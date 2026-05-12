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

/** TCP 시각 동기(MsgType 0x00) Body: 시간 9바이트(yyyy,MM,dd,HH,mm,ss,ms). */
void time_sync_from_tcp_timesync_body(const uint8_t *body, size_t body_len);

/** 현재 시간을 프로토콜 9바이트 형식으로 인코딩. */
void time_encode_now_9(uint8_t out[9]);

#endif /* TIME_HELPER_H */
