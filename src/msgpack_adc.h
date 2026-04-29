/*
 * Smart Gateway - MessagePack ADC 인코더
 *
 * adc_snapshot_t를 MessagePack 형식으로 직렬화합니다.
 * 모든 float32는 소수 둘째 자리로 반올림합니다.
 */

#ifndef MSGPACK_ADC_H
#define MSGPACK_ADC_H

#include "adc.h"
#include <stddef.h>

/*
 * msgpack_encode_adc_snapshot
 *
 * adc_snapshot_t를 MessagePack 바이트 스트림으로 인코딩합니다.
 *
 * [반환값]
 *   >0: 인코딩된 바이트 수
 *   -1: 버퍼 부족 또는 잘못된 입력
 */
int msgpack_encode_adc_snapshot(const adc_snapshot_t *snap, uint8_t *buf, size_t buf_len);

/*
 * 테스트: MessagePack array(4) =
 *   [0] device 문자열(최대 50자), [1] yyyyMMddHHmmssfff(17자),
 *   [2] ch0 float32, [3] ch1 float32 (소수 둘째 자리 반올림)
 */
int msgpack_encode_adc_test_2ch(const adc_snapshot_t *snap, uint8_t *buf, size_t buf_len);

#endif /* MSGPACK_ADC_H */
