/*
 * Smart Gateway - MessagePack ADC 인코더
 *
 * 운영 UDP: DC 00 1C + DeviceID(str) + Timestamp 17자 + V1~8 + min1~8 + max1~8
 *           + Sampling_Count(uint8) + ERROR_Code(uint8)
 * 테스트: fixarray(4) — device, timestamp(17), float, float
 */

#ifndef MSGPACK_ADC_H
#define MSGPACK_ADC_H

#include "adc.h"
#include <stddef.h>

/**
 * adc_snapshot_t → 운영 UDP MessagePack (28필드 순차).
 * @return >0 바이트 수, -1 오류
 */
int msgpack_encode_adc_snapshot(const adc_snapshot_t *snap, uint8_t *buf, size_t buf_len);

/** 테스트: fixarray(4) — 2채널 raw만 */
int msgpack_encode_adc_test_2ch(const adc_snapshot_t *snap, uint8_t *buf, size_t buf_len);

#endif /* MSGPACK_ADC_H */
