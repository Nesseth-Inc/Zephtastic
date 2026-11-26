/*
 * Copyright (c) 2023 Trackunit Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/drivers/gnss/gnss_publish.h>
#include <zephyr/kernel.h>
#include <zephyr/modem/chat.h>

#include <string.h>

#include "gnss_parse.h"
#include "gnss_nmea0183.h"
#include "gnss_nmea0183_match.h"

/* The stock match context is pinned SDK API, so keep the optional RMC course
 * bit in the otherwise bounded RMC UTC comparison word. Parsed UTC is at most
 * 235959999 ms, leaving bit 31 clear. The bit is exposed only synchronously
 * while gnss_publish_data() dispatches the matching immutable data snapshot. */
#define GNSS_NMEA0183_RMC_BEARING_PRESENT BIT(31)
#define GNSS_NMEA0183_RMC_UTC_MASK (~GNSS_NMEA0183_RMC_BEARING_PRESENT)
static const struct gnss_data *publishing_data;
static bool publishing_has_bearing;

bool zt_gnss_nmea0183_match_has_bearing(const struct gnss_data *data)
{
	return data != NULL && data == publishing_data && publishing_has_bearing;
}

static bool gnss_nmea0183_match_valid_message(char **argv, uint16_t argc)
{
	if (argc < 2 || argc > 32) {
		return false;
	}
	for (uint16_t i = 0; i < argc; ++i) {
		/* The retained numeric parser uses int8 indexes and signed nano units.
		 * Bound both total length and whole digits before entering it. */
		if (argv[i] == NULL) {
			return false;
		}
		size_t length = 0;
		while (length < 32 && argv[i][length] != '\0') {
			++length;
		}
		if (length >= 32) {
			return false;
		}
		const char *decimal = memchr(argv[i], '.', length);
		if ((decimal ? (size_t)(decimal - argv[i]) : length) > 9) {
			return false;
		}
	}
	return gnss_nmea0183_validate_message(argv, argc);
}

static int gnss_nmea0183_match_parse_utc(char **argv, uint16_t argc, uint32_t *utc)
{
	int64_t i64;

	if ((gnss_parse_dec_to_milli(argv[1], &i64) < 0) ||
	    (i64 < 0) ||
	    (i64 > UINT32_MAX)) {
		return -EINVAL;
	}

	*utc = (uint32_t)i64;
	return 0;
}

#if CONFIG_GNSS_SATELLITES
static void gnss_nmea0183_match_reset_gsv(struct gnss_nmea0183_match_data *data)
{
	data->satellites_length = 0;
	data->gsv_message_number = 1;
}
#endif

static void gnss_nmea0183_match_publish(struct gnss_nmea0183_match_data *data)
{
	if ((data->gga_utc == UINT32_MAX) || (data->rmc_utc == UINT32_MAX)) {
		return;
	}

	if (data->gga_utc == (data->rmc_utc & GNSS_NMEA0183_RMC_UTC_MASK)) {
		publishing_data = &data->data;
		publishing_has_bearing = (data->rmc_utc & GNSS_NMEA0183_RMC_BEARING_PRESENT) != 0;
		gnss_publish_data(data->gnss, &data->data);
		publishing_data = NULL;
		publishing_has_bearing = false;
	}
}

void gnss_nmea0183_match_gga_callback(struct modem_chat *chat, char **argv, uint16_t argc,
				      void *user_data)
{
	struct gnss_nmea0183_match_data *data = user_data;
	struct gnss_data candidate = data->data;
	uint32_t utc;

	/* Invalidate this half before parsing; failed input cannot reuse its UTC. */
	data->gga_utc = UINT32_MAX;
	if (!gnss_nmea0183_match_valid_message(argv, argc)) {
		return;
	}

	if (gnss_nmea0183_parse_gga((const char **)argv, argc, &candidate) < 0) {
		return;
	}

	if (gnss_nmea0183_match_parse_utc(argv, argc, &utc) < 0) {
		return;
	}

	data->data = candidate;
	data->gga_utc = utc;
	gnss_nmea0183_match_publish(data);
}

void gnss_nmea0183_match_rmc_callback(struct modem_chat *chat, char **argv, uint16_t argc,
				      void *user_data)
{
	struct gnss_nmea0183_match_data *data = user_data;
	struct gnss_data candidate = data->data;
	uint32_t utc;

	/* Invalidate this half before parsing; failed input cannot reuse its UTC. */
	data->rmc_utc = UINT32_MAX;
	if (!gnss_nmea0183_match_valid_message(argv, argc)) {
		return;
	}

	if (gnss_nmea0183_parse_rmc((const char **)argv, argc, &candidate) < 0) {
		return;
	}

	if (gnss_nmea0183_match_parse_utc(argv, argc, &utc) < 0) {
		return;
	}

	data->data = candidate;
	data->rmc_utc = utc | (argv[8][0] != '\0' ? GNSS_NMEA0183_RMC_BEARING_PRESENT : 0u);
	gnss_nmea0183_match_publish(data);
}

#if CONFIG_GNSS_SATELLITES
void gnss_nmea0183_match_gsv_callback(struct modem_chat *chat, char **argv, uint16_t argc,
				      void *user_data)
{
	struct gnss_nmea0183_match_data *data = user_data;
	struct gnss_nmea0183_gsv_header header;
	int ret;

	if (gnss_nmea0183_parse_gsv_header((const char **)argv, argc, &header) < 0) {
		return;
	}

	if (header.number_of_svs == 0) {
		return;
	}

	if (header.message_number != data->gsv_message_number) {
		gnss_nmea0183_match_reset_gsv(data);
		return;
	}

	data->gsv_message_number++;

	ret = gnss_nmea0183_parse_gsv_svs((const char **)argv, argc,
					  &data->satellites[data->satellites_length],
					  data->satellites_size - data->satellites_length);
	if (ret < 0) {
		gnss_nmea0183_match_reset_gsv(data);
		return;
	}

	data->satellites_length += (uint16_t)ret;

	if (data->satellites_length == header.number_of_svs) {
		gnss_publish_satellites(data->gnss, data->satellites, data->satellites_length);
		gnss_nmea0183_match_reset_gsv(data);
	}
}
#endif

int gnss_nmea0183_match_init(struct gnss_nmea0183_match_data *data,
			     const struct gnss_nmea0183_match_config *config)
{
	__ASSERT(data != NULL, "data argument must be provided");
	__ASSERT(config != NULL, "config argument must be provided");

	memset(data, 0, sizeof(struct gnss_nmea0183_match_data));
	/* Midnight UTC zero is valid; keep missing halves separate from its value. */
	data->gga_utc = UINT32_MAX;
	data->rmc_utc = UINT32_MAX;
	data->gnss = config->gnss;
#if CONFIG_GNSS_SATELLITES
	data->satellites = config->satellites;
	data->satellites_size = config->satellites_size;
#endif
	return 0;
}
