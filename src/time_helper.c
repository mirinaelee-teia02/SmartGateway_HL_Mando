/*
 * Smart Gateway - 시간 헬퍼
 * TCP 0x01 SYNC Body 앞 9B로 wall clock 동기
 */

#include "time_helper.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/timeutil.h>

#define BOOT_EPOCH_SEC	1735660800

static struct k_spinlock tl_lock;

static bool wall_synced;
static uint64_t sync_unix_sec;
static uint16_t sync_msec;
static int64_t sync_uptime_ms;

static void seconds_to_datetime(uint32_t sec, datetime_t *dt)
{
	if (!dt) {
		return;
	}

	uint32_t d = sec / 86400U;
	uint32_t r = sec % 86400U;
	uint32_t h = r / 3600U;

	r %= 3600U;
	uint32_t m = r / 60U;
	uint32_t s = r % 60U;

	uint32_t y = 1970;
	uint32_t days_per_year = 365;

	while (d >= days_per_year + ((y % 4U == 0U && y % 100U != 0U) || (y % 400U == 0U))) {
		d -= days_per_year + ((y % 4U == 0U && y % 100U != 0U) || (y % 400U == 0U));
		y++;
	}

	uint8_t md[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

	if ((y % 4U == 0U && y % 100U != 0U) || (y % 400U == 0U)) {
		md[1] = 29;
	}

	uint8_t month = 1;

	for (int i = 0; i < 12; i++) {
		if (d < md[i]) {
			break;
		}
		d -= md[i];
		month++;
	}
	uint8_t day = (uint8_t)(d + 1U);

	dt->year  = (uint16_t)y;
	dt->month = month;
	dt->day   = day;
	dt->hour  = (uint8_t)h;
	dt->min   = (uint8_t)m;
	dt->sec   = (uint8_t)s;
}

void time_sync_from_tcp_timesync_body(const uint8_t *body, size_t body_len)
{
	if (!body || body_len < 9U) {
		return;
	}

	uint16_t year = sys_get_be16(body);
	uint16_t msec = sys_get_be16(body + 7);
	struct tm tm = { 0 };

	tm.tm_year = (int)year - 1900;
	tm.tm_mon = (int)body[2] - 1;
	tm.tm_mday = (int)body[3];
	tm.tm_hour = (int)body[4];
	tm.tm_min = (int)body[5];
	tm.tm_sec = (int)body[6];

	time_t tu = timeutil_timegm(&tm);

	if (tu == (time_t)-1) {
		printf("[TIME] timeutil_timegm failed (server body date check)\n");
		return;
	}

	k_spinlock_key_t key = k_spin_lock(&tl_lock);

	wall_synced = true;
	sync_unix_sec = (uint64_t)tu;
	sync_msec = (msec <= 999U) ? msec : 0U;
	sync_uptime_ms = k_uptime_get();
	k_spin_unlock(&tl_lock, key);

	printf("[TIME] TCP time sync applied: %04u-%02u-%02u %02u:%02u:%02u.%03u UTC (unix %llu)\n",
	       (unsigned)year, (unsigned)body[2], (unsigned)body[3], (unsigned)body[4],
	       (unsigned)body[5], (unsigned)body[6], (unsigned)sync_msec,
	       (unsigned long long)(uint64_t)tu);
}

static void get_now_unix_ms(uint64_t *unix_sec_out, uint16_t *msec_out)
{
	if (!unix_sec_out || !msec_out) {
		return;
	}

	k_spinlock_key_t key = k_spin_lock(&tl_lock);
	bool synced = wall_synced;
	uint64_t base_unix = sync_unix_sec;
	uint16_t base_ms = sync_msec;
	int64_t base_up = sync_uptime_ms;

	k_spin_unlock(&tl_lock, key);

	if (synced) {
		int64_t now = k_uptime_get();
		int64_t delta_ms = now - base_up;

		if (delta_ms < 0) {
			delta_ms = 0;
		}
		uint64_t total_ms = (uint64_t)base_ms + (uint64_t)delta_ms;
		uint64_t unix_sec = base_unix + (total_ms / 1000U);
		uint16_t msec = (uint16_t)(total_ms % 1000U);

		if (unix_sec > 0xFFFFFFFFULL) {
			unix_sec = 0xFFFFFFFFULL;
		}
		*unix_sec_out = unix_sec;
		*msec_out = msec;
		return;
	}

	uint32_t uptime_ms = k_uptime_get_32();

	*unix_sec_out = BOOT_EPOCH_SEC + (uptime_ms / 1000U);
	*msec_out = (uint16_t)(uptime_ms % 1000U);
}

void get_datetime(datetime_t *dt)
{
	if (!dt) {
		return;
	}
	memset(dt, 0, sizeof(*dt));

	uint64_t unix_sec = 0;
	uint16_t msec;

	get_now_unix_ms(&unix_sec, &msec);
	ARG_UNUSED(msec);

	if (unix_sec > 0xFFFFFFFFULL) {
		unix_sec = 0xFFFFFFFFULL;
	}
	seconds_to_datetime((uint32_t)unix_sec, dt);
}

void get_datetime_ms(datetime_t *dt, uint16_t *msec_out)
{
	if (!dt || !msec_out) {
		return;
	}
	memset(dt, 0, sizeof(*dt));

	uint64_t unix_sec = 0;
	uint16_t msec = 0;

	get_now_unix_ms(&unix_sec, &msec);
	if (unix_sec > 0xFFFFFFFFULL) {
		unix_sec = 0xFFFFFFFFULL;
	}
	seconds_to_datetime((uint32_t)unix_sec, dt);
	*msec_out = msec;
}

void time_encode_now_9(uint8_t out[9])
{
	if (!out) {
		return;
	}

	uint64_t unix_sec = 0;
	uint16_t msec = 0;
	datetime_t dt;

	get_now_unix_ms(&unix_sec, &msec);
	if (unix_sec > 0xFFFFFFFFULL) {
		unix_sec = 0xFFFFFFFFULL;
	}
	seconds_to_datetime((uint32_t)unix_sec, &dt);

	sys_put_be16(dt.year, out);
	out[2] = dt.month;
	out[3] = dt.day;
	out[4] = dt.hour;
	out[5] = dt.min;
	out[6] = dt.sec;
	sys_put_be16(msec, out + 7);
}
