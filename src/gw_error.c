/*
 * Smart Gateway — 공통 오류 코드 (UDP ERROR_Code, TCP 0x8E Body)
 */

#include "gw_error.h"

#include <time.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/timeutil.h>

static uint8_t gw_err_code = GW_ERR_NONE;

void gw_error_set(uint8_t code)
{
	if (code > GW_ERR_UDP_COMM) {
		return;
	}
	gw_err_code = code;
}

void gw_error_clear(void)
{
	gw_err_code = GW_ERR_NONE;
}

uint8_t gw_error_get(void)
{
	return gw_err_code;
}

uint8_t gw_rs232_wire_cfg_check(const uint8_t cfg[GW_RS232_CFG_WIRE_LEN])
{
	if (cfg == NULL) {
		return GW_ERR_INIT_CFG;
	}

	switch (cfg[0]) {
	case 0x00:
	case 0x01:
		break;
	default:
		return GW_ERR_INIT_CFG;
	}
	switch (cfg[1]) {
	case 0x00:
	case 0x01:
		break;
	default:
		return GW_ERR_INIT_CFG;
	}
	switch (cfg[2]) {
	case 0x00:
	case 0x01:
		break;
	default:
		return GW_ERR_INIT_CFG;
	}
	switch (cfg[3]) {
	case 0x00:
	case 0x01:
	case 0x02:
		break;
	default:
		return GW_ERR_INIT_CFG;
	}
	switch (cfg[4]) {
	case 0x00:
	case 0x01:
		break;
	default:
		return GW_ERR_INIT_CFG;
	}

	return GW_ERR_NONE;
}

uint8_t gw_sync_body_check(const uint8_t *body, size_t body_len)
{
	if (body == NULL || body_len != GW_SYNC_BODY_LEN) {
		return GW_ERR_INIT_CFG;
	}

	if (gw_rs232_wire_cfg_check(body + GW_SYNC_RS232_OFFSET) != GW_ERR_NONE) {
		return GW_ERR_INIT_CFG;
	}

	uint16_t year = sys_get_be16(body);
	uint8_t mon = body[2];
	uint8_t day = body[3];
	uint8_t hour = body[4];
	uint8_t min = body[5];
	uint8_t sec = body[6];
	uint16_t msec = sys_get_be16(body + 7);

	if (year < 2000U || year > 2099U) {
		return GW_ERR_INIT_CFG;
	}
	if (mon < 1U || mon > 12U) {
		return GW_ERR_INIT_CFG;
	}
	if (day < 1U || day > 31U) {
		return GW_ERR_INIT_CFG;
	}
	if (hour > 23U || min > 59U || sec > 59U) {
		return GW_ERR_INIT_CFG;
	}
	if (msec > 999U) {
		return GW_ERR_INIT_CFG;
	}

	struct tm tm = { 0 };

	tm.tm_year = (int)year - 1900;
	tm.tm_mon = (int)mon - 1;
	tm.tm_mday = (int)day;
	tm.tm_hour = (int)hour;
	tm.tm_min = (int)min;
	tm.tm_sec = (int)sec;

	if (timeutil_timegm(&tm) == (time_t)-1) {
		return GW_ERR_INIT_CFG;
	}

	return GW_ERR_NONE;
}
