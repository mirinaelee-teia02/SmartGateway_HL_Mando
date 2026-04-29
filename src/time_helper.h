/*
 * Smart Gateway - 시간 헬퍼
 * datetime_t: TCP MsgType 0x00 동기 후 서버 시각, 없으면 부트 epoch + uptime
 */

#ifndef TIME_HELPER_H
#define TIME_HELPER_H

#include "adc.h"

#include <stddef.h>
#include <stdint.h>

/** 현재 시각을 datetime_t로 채움 (ADC·UDP 타임스탬프용) */
void get_datetime(datetime_t *dt);

/** TCP 시각 동기(MsgType 0x00) Body (7바이트 이상). */
void time_sync_from_tcp_timesync_body(const uint8_t *body, size_t body_len);

#endif /* TIME_HELPER_H */
