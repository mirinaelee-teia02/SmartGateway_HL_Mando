/*
 * Smart Gateway - 시간 헬퍼
 * TCP 0x00 본문으로 wall clock 동기 후 get_datetime에서 경과 초 반영
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
	if (!body || body_len < 7U) {
		return;
	}

	uint16_t year = sys_get_be16(body);
	struct tm tm = { 0 };

	tm.tm_year = (int)year - 1900;
	tm.tm_mon = (int)body[2] - 1;
	tm.tm_mday = (int)body[3];
	tm.tm_hour = (int)body[4];
	tm.tm_min = (int)body[5];
	tm.tm_sec = (int)body[6];

	time_t tu = timeutil_timegm(&tm);

	if (tu == (time_t)-1) {
		printf("[TIME] timeutil_timegm 실패 (서버 Body 날짜 검사)\n");
		return;
	}

	k_spinlock_key_t key = k_spin_lock(&tl_lock);

	wall_synced = true;
	sync_unix_sec = (uint64_t)tu;
	sync_uptime_ms = k_uptime_get();
	k_spin_unlock(&tl_lock, key);

	printf("[TIME] TCP 시각동기 적용: %04u-%02u-%02u %02u:%02u:%02u UTC (unix %llu)\n",
	       (unsigned)year, (unsigned)body[2], (unsigned)body[3], (unsigned)body[4],
	       (unsigned)body[5], (unsigned)body[6], (unsigned long long)(uint64_t)tu);
}

void get_datetime(datetime_t *dt)
{
	if (!dt) {
		return;
	}
	memset(dt, 0, sizeof(*dt));

	k_spinlock_key_t key = k_spin_lock(&tl_lock);
	bool synced = wall_synced;
	uint64_t base_unix = sync_unix_sec;
	int64_t base_up = sync_uptime_ms;

	k_spin_unlock(&tl_lock, key);

	if (synced) {
		int64_t now = k_uptime_get();
		int64_t delta_ms = now - base_up;

		if (delta_ms < 0) {
			delta_ms = 0;
		}
		uint64_t elapsed = (uint64_t)(delta_ms / 1000);
		uint64_t unix_sec = base_unix + elapsed;

		if (unix_sec > 0xFFFFFFFFULL) {
			unix_sec = 0xFFFFFFFFULL;
		}
		seconds_to_datetime((uint32_t)unix_sec, dt);
		return;
	}

	uint32_t uptime_sec = k_uptime_get_32() / 1000U;
	uint32_t unix_sec = BOOT_EPOCH_SEC + uptime_sec;

	seconds_to_datetime(unix_sec, dt);
}
