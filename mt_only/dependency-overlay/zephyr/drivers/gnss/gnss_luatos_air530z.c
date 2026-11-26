/*
 * Copyright (c) 2024 Jerónimo Agulló
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/drivers/gnss.h>
#include <zephyr/drivers/gnss/gnss_publish.h>
#include <zephyr/modem/chat.h>
#include <zephyr/modem/backend/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <string.h>

#include "gnss_nmea0183.h"
#include "gnss_nmea0183_match.h"
#include "gnss_parse.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(luatos_air530z, CONFIG_GNSS_LOG_LEVEL);

#define DT_DRV_COMPAT luatos_air530z

#define UART_RECV_BUF_SZ 128
#define UART_TRANS_BUF_SZ 64

#define CHAT_RECV_BUF_SZ 256
#define CHAT_ARGV_SZ 32

/* Inter-command delay. These parts are CASIC silicon on a 9600-baud link and
 * some commands (constellation selection especially) restart the navigation
 * engine, so the module needs real settling time between sentences. 10 ms is
 * not enough — a command issued into a restarting engine is simply lost.
 */
#define AIR530Z_CMD_DELAY_MS 250

/* Navigation dynamic model — $PCAS11,<mode>. Undocumented in the L76K
 * protocol specification (which lists only PCAS01/02/03/04/10) but accepted
 * by these parts in practice. Mode 1 = stationary is confirmed against a
 * published CASIC command reference; the remaining values follow the usual
 * dynamic-model ladder.
 *
 * Worth setting explicitly even though it looks like a tuning detail: the
 * dynamic model PERSISTS IN THE MODULE across firmware flashes, so a board
 * that previously ran other GNSS firmware can arrive stuck in an airborne or
 * automotive model that quietly degrades fixes for a stationary node.
 */
#if CONFIG_GNSS_LUATOS_AIR530Z_NAV_MODE == 0
#define AIR530Z_NAV_CMD "$PCAS11,0*1D"
#elif CONFIG_GNSS_LUATOS_AIR530Z_NAV_MODE == 1
#define AIR530Z_NAV_CMD "$PCAS11,1*1C"
#elif CONFIG_GNSS_LUATOS_AIR530Z_NAV_MODE == 2
#define AIR530Z_NAV_CMD "$PCAS11,2*1F"
#elif CONFIG_GNSS_LUATOS_AIR530Z_NAV_MODE == 3
#define AIR530Z_NAV_CMD "$PCAS11,3*1E"
#elif CONFIG_GNSS_LUATOS_AIR530Z_NAV_MODE == 4
#define AIR530Z_NAV_CMD "$PCAS11,4*19"
#elif CONFIG_GNSS_LUATOS_AIR530Z_NAV_MODE == 5
#define AIR530Z_NAV_CMD "$PCAS11,5*18"
#elif CONFIG_GNSS_LUATOS_AIR530Z_NAV_MODE == 6
#define AIR530Z_NAV_CMD "$PCAS11,6*1B"
#elif CONFIG_GNSS_LUATOS_AIR530Z_NAV_MODE == 7
#define AIR530Z_NAV_CMD "$PCAS11,7*1A"
#endif

MODEM_CHAT_SCRIPT_CMDS_DEFINE(init_script_cmds,
#if CONFIG_GNSS_SATELLITES
	/* receive only GGA, RMC and GSV NMEA messages */
	MODEM_CHAT_SCRIPT_CMD_RESP_NONE("$PCAS03,1,0,0,1,1,0,0,0,0,0,0,0,0*1F",
					AIR530Z_CMD_DELAY_MS),
#else
	/* receive only GGA and RMC NMEA messages */
	MODEM_CHAT_SCRIPT_CMD_RESP_NONE("$PCAS03,1,0,0,0,1,0,0,0,0,0,0,0,0*1E",
					AIR530Z_CMD_DELAY_MS),
#endif
#ifdef AIR530Z_NAV_CMD
	MODEM_CHAT_SCRIPT_CMD_RESP_NONE(AIR530Z_NAV_CMD, AIR530Z_CMD_DELAY_MS),
#endif
#if IS_ENABLED(CONFIG_GNSS_LUATOS_AIR530Z_VERSION_QUERY)
	/* Ask the module to identify itself. The reply arrives in-band as an
	 * ordinary $GPTXT NMEA sentence (see the unsolicited match below), so
	 * it costs nothing to read and needs no raw access to the UART.
	 *
	 * This is the only positive proof available on this transport that the
	 * MCU's TX line actually reaches the module: every other command here
	 * is written blind. If configuration appears to have no effect, a
	 * missing version string says the module never heard any of it.
	 */
	MODEM_CHAT_SCRIPT_CMD_RESP_NONE("$PCAS06,0*1B", AIR530Z_CMD_DELAY_MS),
#endif
);

/* NOTE: configuration is deliberately NOT persisted to the module ($PCAS00).
 * The init script re-sends it on every boot, and a saved configuration is
 * sticky — including a wrong one, which then survives reflashing and has to
 * be hunted down on hardware.
 */

MODEM_CHAT_SCRIPT_NO_ABORT_DEFINE(init_script, init_script_cmds, NULL, 5);

struct gnss_luatos_air530z_config {
	const struct device *uart;
	const struct gpio_dt_spec on_off_gpio;
	const int uart_baudrate;
};

struct gnss_luatos_air530z_data {
	struct gnss_nmea0183_match_data match_data;
#if CONFIG_GNSS_SATELLITES
	struct gnss_satellite satellites[CONFIG_GNSS_LUATOS_AIR530Z_SATELLITES_COUNT];
#endif

	/* UART backend */
	struct modem_pipe *uart_pipe;
	struct modem_backend_uart uart_backend;
	uint8_t uart_backend_receive_buf[UART_RECV_BUF_SZ];
	uint8_t uart_backend_transmit_buf[UART_TRANS_BUF_SZ];

	/* Modem chat */
	struct modem_chat chat;
	uint8_t chat_receive_buf[CHAT_RECV_BUF_SZ];
	uint8_t chat_delimiter[2];
	uint8_t *chat_argv[CHAT_ARGV_SZ];

	/* Dynamic chat script */
	uint8_t dynamic_separators_buf[2];
	uint8_t dynamic_request_buf[32];
	struct modem_chat_script_chat dynamic_script_chat;
	struct modem_chat_script dynamic_script;

	struct k_sem lock;
};

#if IS_ENABLED(CONFIG_GNSS_LUATOS_AIR530Z_VERSION_QUERY)
/* Module identification string, captured from the $PCAS06 reply.
 *
 * $GPTXT,01,01,02,SW=<version>,... — the interesting field is the one
 * starting "SW=". Stored verbatim (truncated) rather than parsed: this is a
 * diagnostic, and different parts word the reply differently.
 */
static char air530z_version[40];

static void air530z_gptxt_callback(struct modem_chat *chat, char **argv, uint16_t argc,
				   void *user_data)
{
	ARG_UNUSED(chat);
	ARG_UNUSED(user_data);

	/* Take the first field that looks like a software-version token. */
	for (uint16_t i = 1; i < argc; i++) {
		if (strncmp(argv[i], "SW=", 3) == 0) {
			strncpy(air530z_version, argv[i] + 3, sizeof(air530z_version) - 1);
			air530z_version[sizeof(air530z_version) - 1] = '\0';
			LOG_INF("GNSS module version: %s", air530z_version);
			return;
		}
	}
}

/* Exported for application-level diagnostics. Returns the number of
 * characters written, or 0 if the module has not identified itself — which
 * is itself the useful signal (no reply => the module is not receiving).
 */
__weak int zephcore_gnss_version_get(char *buf, size_t len)
{
	if (buf == NULL || len == 0 || air530z_version[0] == '\0') {
		return 0;
	}

	strncpy(buf, air530z_version, len - 1);
	buf[len - 1] = '\0';
	return (int)strlen(buf);
}
#endif /* CONFIG_GNSS_LUATOS_AIR530Z_VERSION_QUERY */

/* Raw sentence counter — see the equivalent note in gnss_nmea_generic.c.
 * Non-zero means the module is alive, at the right baud, and emitting
 * parseable NMEA; zero means nothing is arriving at all. */
static uint32_t air530z_rx_sentences;

static void air530z_gga_cb(struct modem_chat *chat, char **argv, uint16_t argc,
			   void *user_data)
{
	air530z_rx_sentences++;
	gnss_nmea0183_match_gga_callback(chat, argv, argc, user_data);
}

static void air530z_rmc_cb(struct modem_chat *chat, char **argv, uint16_t argc,
			   void *user_data)
{
	air530z_rx_sentences++;
	gnss_nmea0183_match_rmc_callback(chat, argv, argc, user_data);
}

__weak uint32_t zephcore_gnss_rx_count(void)
{
	return air530z_rx_sentences;
}

MODEM_CHAT_MATCHES_DEFINE(unsol_matches,
	MODEM_CHAT_MATCH_WILDCARD("$??GGA,", ",*", air530z_gga_cb),
	MODEM_CHAT_MATCH_WILDCARD("$??RMC,", ",*", air530z_rmc_cb),
#if CONFIG_GNSS_SATELLITES
	MODEM_CHAT_MATCH_WILDCARD("$??GSV,", ",*", gnss_nmea0183_match_gsv_callback),
#endif
#if IS_ENABLED(CONFIG_GNSS_LUATOS_AIR530Z_VERSION_QUERY)
	MODEM_CHAT_MATCH_WILDCARD("$??TXT,", ",*", air530z_gptxt_callback),
#endif
);

static void luatos_air530z_lock(const struct device *dev)
{
	struct gnss_luatos_air530z_data *data = dev->data;

	(void)k_sem_take(&data->lock, K_FOREVER);
}

static void luatos_air530z_unlock(const struct device *dev)
{
	struct gnss_luatos_air530z_data *data = dev->data;

	k_sem_give(&data->lock);
}

static int gnss_luatos_air530z_init_nmea0183_match(const struct device *dev)
{
	struct gnss_luatos_air530z_data *data = dev->data;

	const struct gnss_nmea0183_match_config match_config = {
		.gnss = dev,
#if CONFIG_GNSS_SATELLITES
		.satellites = data->satellites,
		.satellites_size = ARRAY_SIZE(data->satellites),
#endif
	};

	return gnss_nmea0183_match_init(&data->match_data, &match_config);
}

static void gnss_luatos_air530z_init_pipe(const struct device *dev)
{
	const struct gnss_luatos_air530z_config *config = dev->config;
	struct gnss_luatos_air530z_data *data = dev->data;

	const struct modem_backend_uart_config uart_backend_config = {
		.uart = config->uart,
		.receive_buf = data->uart_backend_receive_buf,
		.receive_buf_size = sizeof(data->uart_backend_receive_buf),
		.transmit_buf = data->uart_backend_transmit_buf,
		.transmit_buf_size = ARRAY_SIZE(data->uart_backend_transmit_buf),
	};

	data->uart_pipe = modem_backend_uart_init(&data->uart_backend, &uart_backend_config);
}

static int gnss_luatos_air530z_init_chat(const struct device *dev)
{
	struct gnss_luatos_air530z_data *data = dev->data;

	const struct modem_chat_config chat_config = {
		.user_data = data,
		.receive_buf = data->chat_receive_buf,
		.receive_buf_size = sizeof(data->chat_receive_buf),
		.delimiter = data->chat_delimiter,
		.delimiter_size = ARRAY_SIZE(data->chat_delimiter),
		.filter = NULL,
		.filter_size = 0,
		.argv = data->chat_argv,
		.argv_size = ARRAY_SIZE(data->chat_argv),
		.unsol_matches = unsol_matches,
		.unsol_matches_size = ARRAY_SIZE(unsol_matches),
	};

	return modem_chat_init(&data->chat, &chat_config);
}

static void luatos_air530z_init_dynamic_script(const struct device *dev)
{
	struct gnss_luatos_air530z_data *data = dev->data;

	/* Air530z doesn't respond to commands. Thus, response_matches_size = 0; */
	data->dynamic_script_chat.request = data->dynamic_request_buf;
	data->dynamic_script_chat.response_matches = NULL;
	data->dynamic_script_chat.response_matches_size = 0;
	data->dynamic_script_chat.timeout = 0;

	data->dynamic_script.name = "PCAS";
	data->dynamic_script.script_chats = &data->dynamic_script_chat;
	data->dynamic_script.script_chats_size = 1;
	data->dynamic_script.abort_matches = NULL;
	data->dynamic_script.abort_matches_size = 0;
	data->dynamic_script.callback = NULL;
	data->dynamic_script.timeout = 5;
}

static int gnss_luatos_air530z_init(const struct device *dev)
{
	struct gnss_luatos_air530z_data *data = dev->data;
	const struct gnss_luatos_air530z_config *config = dev->config;
	int ret;

	k_sem_init(&data->lock, 1, 1);

	ret = gnss_luatos_air530z_init_nmea0183_match(dev);
	if (ret < 0) {
		return ret;
	}

	gnss_luatos_air530z_init_pipe(dev);

	ret = gnss_luatos_air530z_init_chat(dev);
	if (ret < 0) {
		return ret;
	}

	luatos_air530z_init_dynamic_script(dev);

	ret = modem_pipe_open(data->uart_pipe, K_SECONDS(10));
	if (ret < 0) {
		return ret;
	}

	ret = modem_chat_attach(&data->chat, data->uart_pipe);
	if (ret < 0) {
		modem_pipe_close(data->uart_pipe, K_SECONDS(10));
		return ret;
	}

	ret = modem_chat_run_script(&data->chat, &init_script);
	if (ret < 0) {
		LOG_ERR("Failed to run init_script");
		modem_pipe_close(data->uart_pipe, K_SECONDS(10));
		return ret;
	}

	/* setup on-off gpio for power management */
	if (!gpio_is_ready_dt(&config->on_off_gpio)) {
		LOG_ERR("on-off GPIO device not ready");
		return -ENODEV;
	}

	gpio_pin_configure_dt(&config->on_off_gpio, GPIO_OUTPUT_HIGH);

	return 0;
}

/* PM hooks intentionally removed — the L76K ignores $PMTK161,0 (AT6558-
 * based, not genuine MediaTek), so PM-driven software sleep can never
 * work; standby drives the FORCE_ON GPIO and the app suspends the UART
 * instead (ZephyrGPSManager, main thread only). PM-less also keeps
 * modem_chat scripts out of PM callbacks (deadlock risk off sysworkq). */

static int luatos_air530z_set_fix_rate(const struct device *dev, uint32_t fix_interval_ms)
{
	struct gnss_luatos_air530z_data *data = dev->data;
	int ret;

	if (fix_interval_ms < 100 || fix_interval_ms > 1000) {
		return -EINVAL;
	}

	luatos_air530z_lock(dev);

	ret = gnss_nmea0183_snprintk(data->dynamic_request_buf, sizeof(data->dynamic_request_buf),
				     "PCAS02,%u", fix_interval_ms);
	if (ret < 0) {
		goto unlock_return;
	}

	data->dynamic_script_chat.request_size = ret;

	ret = modem_chat_run_script(&data->chat, &data->dynamic_script);
	if (ret < 0) {
		goto unlock_return;
	}

unlock_return:
	luatos_air530z_unlock(dev);
	return ret;
}

static int luatos_air530z_set_enabled_systems(const struct device *dev, gnss_systems_t systems)
{
	struct gnss_luatos_air530z_data *data = dev->data;
	gnss_systems_t supported_systems;
	uint8_t encoded_systems = 0;
	int ret;

	supported_systems = (GNSS_SYSTEM_GPS | GNSS_SYSTEM_GLONASS | GNSS_SYSTEM_BEIDOU);

	if ((~supported_systems) & systems) {
		return -EINVAL;
	}

	luatos_air530z_lock(dev);

	WRITE_BIT(encoded_systems, 0, systems & GNSS_SYSTEM_GPS);
	WRITE_BIT(encoded_systems, 1, systems & GNSS_SYSTEM_GLONASS);
	WRITE_BIT(encoded_systems, 2, systems & GNSS_SYSTEM_BEIDOU);

	ret = gnss_nmea0183_snprintk(data->dynamic_request_buf, sizeof(data->dynamic_request_buf),
					"PCAS04,%u", encoded_systems);
	if (ret < 0) {
		goto unlock_return;
	}

	data->dynamic_script_chat.request_size = ret;

	ret = modem_chat_run_script(&data->chat, &data->dynamic_script);
	if (ret < 0) {
		goto unlock_return;
	}

unlock_return:
	luatos_air530z_unlock(dev);
	return ret;

}

static int luatos_air530z_get_supported_systems(const struct device *dev, gnss_systems_t *systems)
{
	*systems = (GNSS_SYSTEM_GPS | GNSS_SYSTEM_GLONASS | GNSS_SYSTEM_BEIDOU);
	return 0;
}

static DEVICE_API(gnss, gnss_api) = {
	.set_fix_rate = luatos_air530z_set_fix_rate,
	.set_enabled_systems = luatos_air530z_set_enabled_systems,
	.get_supported_systems = luatos_air530z_get_supported_systems,
};

#define LUATOS_AIR530Z(inst)									\
	static const struct gnss_luatos_air530z_config gnss_luatos_air530z_cfg_##inst = {	\
		.uart = DEVICE_DT_GET(DT_INST_BUS(inst)),					\
		.on_off_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, on_off_gpios, { 0 }),		\
	};											\
												\
	static struct gnss_luatos_air530z_data gnss_luatos_air530z_data_##inst = {		\
		.chat_delimiter = {'\r', '\n'},							\
		.dynamic_separators_buf = {',', '*'},						\
	};											\
												\
	DEVICE_DT_INST_DEFINE(inst, gnss_luatos_air530z_init,					\
		NULL,										\
		&gnss_luatos_air530z_data_##inst,						\
		&gnss_luatos_air530z_cfg_##inst,						\
		POST_KERNEL, CONFIG_GNSS_INIT_PRIORITY, &gnss_api);

DT_INST_FOREACH_STATUS_OKAY(LUATOS_AIR530Z)
