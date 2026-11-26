/*
 * Copyright (c) 2023 Trackunit Corporation
 * Copyright (c) 2023 Bjarki Arge Andreasen
 * Copyright 2023 Google LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/drivers/gnss.h>
#include <zephyr/drivers/gnss/gnss_publish.h>
#include <zephyr/modem/chat.h>
#include <zephyr/modem/backend/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/atomic.h>
#include <string.h>

#include "gnss_nmea0183.h"
#include "gnss_nmea0183_match.h"
#include "gnss_parse.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(gnss_nmea_generic, CONFIG_GNSS_LOG_LEVEL);

#define UART_RX_BUF_SZ (256 + IS_ENABLED(CONFIG_GNSS_SATELLITES) * 512)
#define UART_TX_BUF_SZ 64
#define CHAT_RECV_BUF_SZ 256
#define CHAT_ARGV_SZ 32

struct gnss_nmea_generic_config {
	const struct device *uart;
	const struct modem_chat_script *const init_chat_script;
};

struct gnss_nmea_generic_data {
	struct gnss_nmea0183_match_data match_data;
#if CONFIG_GNSS_SATELLITES
	struct gnss_satellite satellites[CONFIG_GNSS_NMEA_GENERIC_SATELLITES_COUNT];
#endif

	/* UART backend */
	struct modem_pipe *uart_pipe;
	struct modem_backend_uart uart_backend;
	uint8_t uart_backend_receive_buf[UART_RX_BUF_SZ];
	uint8_t uart_backend_transmit_buf[UART_TX_BUF_SZ];

	/* Modem chat */
	struct modem_chat chat;
	uint8_t chat_receive_buf[CHAT_RECV_BUF_SZ];
	uint8_t *chat_argv[CHAT_ARGV_SZ];
};

/* Module identification, captured from a $GPTXT sentence.
 *
 * This driver is a passive listener — every configuration command the
 * application sends is written blind, and nothing on this transport confirms
 * the module received any of it. A CASIC part answers a $PCAS06 version query
 * in-band with $GPTXT, and u-blox emit $GNTXT boot banners unprompted, so a
 * captured string here is the one positive proof that the MCU's TX line
 * reaches the module (or, for the u-blox banner, that RX is alive and the
 * part is identified). Absence is the diagnostic signal.
 */
static char nmea_generic_version[40];

static void nmea_generic_txt_callback(struct modem_chat *chat, char **argv, uint16_t argc,
				      void *user_data)
{
	ARG_UNUSED(chat);
	ARG_UNUSED(user_data);

	/* Only an explicit software-version token counts as an identity. */
	for (uint16_t i = 1; i < argc; i++) {
		if (strncmp(argv[i], "SW=", 3) == 0) {
			strncpy(nmea_generic_version, argv[i] + 3,
				sizeof(nmea_generic_version) - 1);
			nmea_generic_version[sizeof(nmea_generic_version) - 1] = '\0';
			LOG_INF("GNSS module version: %s", nmea_generic_version);
			return;
		}
	}

	/* Deliberately NO generic fallback to "keep the last text field".
	 * TXT sentences also carry warnings and errors — a module complaining
	 * about a sentence it did not understand was being captured and then
	 * reported as though it were the module's identity. A diagnostic that
	 * invents an answer is worse than one admitting it has none, so an
	 * unrecognised TXT is ignored and the field reads as no-reply. */
}

/* u-blox reply to a $PUBX poll. u-blox ignore the CASIC $PCAS06 query, so
 * without this a u-blox module always reads as "no reply" and the TX-path
 * proof is unavailable on exactly the boards that most need it. The content
 * is time/status, not a version — what matters is that a reply arrived at
 * all, which is what proves the module received our command. */
static void nmea_generic_pubx_callback(struct modem_chat *chat, char **argv, uint16_t argc,
				       void *user_data)
{
	ARG_UNUSED(chat);
	ARG_UNUSED(argv);
	ARG_UNUSED(argc);
	ARG_UNUSED(user_data);

	if (nmea_generic_version[0] == '\0') {
		strcpy(nmea_generic_version, "u-blox");
		LOG_INF("GNSS module replied to $PUBX poll (u-blox family)");
	}
}

/* Exported for application-level diagnostics. Returns characters written, or
 * 0 if the module never identified itself. Weak so a board that also builds
 * another GNSS driver exporting this symbol still links. */
__weak int zephcore_gnss_version_get(char *buf, size_t len)
{
	if (buf == NULL || len == 0 || nmea_generic_version[0] == '\0') {
		return 0;
	}

	strncpy(buf, nmea_generic_version, len - 1);
	buf[len - 1] = '\0';
	return (int)strlen(buf);
}

/* Raw sentence dump for field diagnosis (CONFIG_ZEPHCORE_GPS_NMEA_DUMP).
 *
 * Reconstructs the sentence from the parsed argv and logs it. Enabled only
 * for deliberate debugging: at 1 Hz with multi-GNSS this is a lot of console
 * traffic, and on a repeater the console is shared with the CLI.
 *
 * Matching is first-match-wins, so the ordinary GGA/RMC/GSV matches never
 * reach a catch-all placed after them — hence the logging lives in the
 * wrappers as well as in the catch-all, which then covers everything else the
 * module emits (TXT banners, PUBX replies, sentences we do not parse).
 */
#ifdef CONFIG_ZEPHCORE_GPS_NMEA_DUMP
static void nmea_generic_dump(char **argv, uint16_t argc)
{
	char line[128];
	size_t n = 0;

	for (uint16_t i = 0; i < argc && n < sizeof(line) - 2; i++) {
		n += (size_t)snprintk(&line[n], sizeof(line) - n, "%s%s",
				      (i == 0) ? "" : ",", argv[i]);
	}
	LOG_INF("NMEA: %s", line);
}

static void nmea_generic_catchall_cb(struct modem_chat *chat, char **argv, uint16_t argc,
				     void *user_data)
{
	ARG_UNUSED(chat);
	ARG_UNUSED(user_data);
	nmea_generic_dump(argv, argc);
}
#else
#define nmea_generic_dump(argv, argc) do { } while (0)
#endif

/* Raw sentence counter — the only unambiguous "is the module talking to us"
 * signal available.
 *
 * Everything else can be misread. No satellites, a module mute because its
 * output protocol was disabled, and a module stranded at the wrong baud all
 * present identically to the application: zero satellites and no fix. Even
 * the position callback cannot tell them apart, because the NMEA matcher only
 * publishes once GGA and RMC agree on a valid UTC — so a receiver that is
 * healthily emitting NMEA while it searches the sky publishes nothing at all.
 *
 * Counting parsed sentences separates the cases outright: non-zero means the
 * module is alive, at the right baud, and emitting NMEA we can parse, so any
 * remaining problem is signal. Zero means nothing is arriving and no amount
 * of antenna work will help.
 */
static atomic_t nmea_rx_sentences;

static void nmea_generic_gga_cb(struct modem_chat *chat, char **argv, uint16_t argc,
				void *user_data)
{
	atomic_inc(&nmea_rx_sentences);
	nmea_generic_dump(argv, argc);
	gnss_nmea0183_match_gga_callback(chat, argv, argc, user_data);
}

static void nmea_generic_rmc_cb(struct modem_chat *chat, char **argv, uint16_t argc,
				void *user_data)
{
	atomic_inc(&nmea_rx_sentences);
	nmea_generic_dump(argv, argc);
	gnss_nmea0183_match_rmc_callback(chat, argv, argc, user_data);
}

/* Exported for application-level diagnostics. Weak so boards building another
 * GNSS driver that also exports it still link. */
__weak uint32_t zephcore_gnss_rx_count(void)
{
	return (uint32_t)atomic_get(&nmea_rx_sentences);
}

MODEM_CHAT_MATCHES_DEFINE(unsol_matches,
	MODEM_CHAT_MATCH_WILDCARD("$??GGA,", ",*", nmea_generic_gga_cb),
	MODEM_CHAT_MATCH_WILDCARD("$??RMC,", ",*", nmea_generic_rmc_cb),
#if CONFIG_GNSS_SATELLITES
	MODEM_CHAT_MATCH_WILDCARD("$??GSV,", ",*", gnss_nmea0183_match_gsv_callback),
#endif
	MODEM_CHAT_MATCH_WILDCARD("$??TXT,", ",*", nmea_generic_txt_callback),
	MODEM_CHAT_MATCH("$PUBX", ",*", nmea_generic_pubx_callback),
#ifdef CONFIG_ZEPHCORE_GPS_NMEA_DUMP
	/* Must stay LAST: a zero-length match matches every line, so anything
	 * after it would be unreachable. */
	MODEM_CHAT_MATCH("", ",*", nmea_generic_catchall_cb),
#endif
);

static int gnss_nmea_generic_resume(const struct device *dev)
{
	const struct gnss_nmea_generic_config *cfg = dev->config;
	struct gnss_nmea_generic_data *data = dev->data;
	int ret;

	ret = modem_pipe_open(data->uart_pipe, K_SECONDS(10));
	if (ret < 0) {
		return ret;
	}

	ret = modem_chat_attach(&data->chat, data->uart_pipe);

	if (ret == 0) {
		ret = modem_chat_run_script(&data->chat, cfg->init_chat_script);
	}

	if (ret < 0) {
		modem_pipe_close(data->uart_pipe, K_SECONDS(10));
	}
	return ret;
}

#if CONFIG_PM_DEVICE
static int gnss_nmea_generic_suspend(const struct device *dev)
{
	struct gnss_nmea_generic_data *data = dev->data;

	modem_chat_release(&data->chat);
	return modem_pipe_close(data->uart_pipe, K_SECONDS(10));
}
#endif

static DEVICE_API(gnss, gnss_api) = {
};

static int gnss_nmea_generic_init_nmea0183_match(const struct device *dev)
{
	struct gnss_nmea_generic_data *data = dev->data;

	const struct gnss_nmea0183_match_config match_config = {
		.gnss = dev,
#if CONFIG_GNSS_SATELLITES
		.satellites = data->satellites,
		.satellites_size = ARRAY_SIZE(data->satellites),
#endif
	};

	return gnss_nmea0183_match_init(&data->match_data, &match_config);
}

static void gnss_nmea_generic_init_pipe(const struct device *dev)
{
	const struct gnss_nmea_generic_config *cfg = dev->config;
	struct gnss_nmea_generic_data *data = dev->data;

	const struct modem_backend_uart_config uart_backend_config = {
		.uart = cfg->uart,
		.receive_buf = data->uart_backend_receive_buf,
		.receive_buf_size = sizeof(data->uart_backend_receive_buf),
		.transmit_buf = data->uart_backend_transmit_buf,
		.transmit_buf_size = sizeof(data->uart_backend_transmit_buf),
	};

	data->uart_pipe = modem_backend_uart_init(&data->uart_backend, &uart_backend_config);
}

static uint8_t gnss_nmea_generic_char_delimiter[] = {'\r', '\n'};

static int gnss_nmea_generic_init_chat(const struct device *dev)
{
	struct gnss_nmea_generic_data *data = dev->data;

	const struct modem_chat_config chat_config = {
		.user_data = data,
		.receive_buf = data->chat_receive_buf,
		.receive_buf_size = sizeof(data->chat_receive_buf),
		.delimiter = gnss_nmea_generic_char_delimiter,
		.delimiter_size = ARRAY_SIZE(gnss_nmea_generic_char_delimiter),
		.filter = NULL,
		.filter_size = 0,
		.argv = data->chat_argv,
		.argv_size = ARRAY_SIZE(data->chat_argv),
		.unsol_matches = unsol_matches,
		.unsol_matches_size = ARRAY_SIZE(unsol_matches),
	};

	return modem_chat_init(&data->chat, &chat_config);
}

static int gnss_nmea_generic_init(const struct device *dev)
{
	int ret;

	ret = gnss_nmea_generic_init_nmea0183_match(dev);
	if (ret < 0) {
		return ret;
	}

	gnss_nmea_generic_init_pipe(dev);

	ret = gnss_nmea_generic_init_chat(dev);
	if (ret < 0) {
		return ret;
	}

#if CONFIG_PM_DEVICE
	pm_device_init_suspended(dev);
#else
	ret = gnss_nmea_generic_resume(dev);
	if (ret < 0) {
		return ret;
	}
#endif

	return 0;
}

#if CONFIG_PM_DEVICE
static int gnss_nmea_generic_pm_action(const struct device *dev, enum pm_device_action action)
{
	switch (action) {
	case PM_DEVICE_ACTION_RESUME:
		return gnss_nmea_generic_resume(dev);
	case PM_DEVICE_ACTION_SUSPEND:
		return gnss_nmea_generic_suspend(dev);
	default:
		return -ENOTSUP;
	}
}
#endif

#if DT_HAS_COMPAT_STATUS_OKAY(gnss_nmea_generic)
MODEM_CHAT_SCRIPT_EMPTY_DEFINE(gnss_nmea_generic_init_chat_script);
#endif

#define GNSS_NMEA_GENERIC(inst)								\
	static const struct gnss_nmea_generic_config gnss_nmea_generic_cfg_##inst = {	\
		.uart = DEVICE_DT_GET(DT_INST_BUS(inst)),				\
		.init_chat_script = &_CONCAT(DT_DRV_COMPAT, _init_chat_script),         \
	};										\
											\
	static struct gnss_nmea_generic_data gnss_nmea_generic_data_##inst;		\
											\
	PM_DEVICE_DT_INST_DEFINE(inst, gnss_nmea_generic_pm_action);                    \
                                                                                        \
	DEVICE_DT_INST_DEFINE(inst, gnss_nmea_generic_init, PM_DEVICE_DT_INST_GET(inst),\
			      &gnss_nmea_generic_data_##inst,				\
			      &gnss_nmea_generic_cfg_##inst,				\
			      POST_KERNEL, CONFIG_GNSS_INIT_PRIORITY, &gnss_api);

#define DT_DRV_COMPAT gnss_nmea_generic
DT_INST_FOREACH_STATUS_OKAY(GNSS_NMEA_GENERIC)
#undef DT_DRV_COMPAT
