#ifndef DIP_SWITCH_H
#define DIP_SWITCH_H

#include <stdint.h>

/*
 * DIP 스위치 동작 규칙:
 *   SW1~SW7: 7비트 이진수 → 장치코드 인덱스 (0~127)
 *     0 (전부 OFF) → NVS 저장값 사용
 *     1~127       → dip_code_table[] 에서 장치코드 선택
 *
 *   SW8: 장치 인덱스
 *     OFF(0) → device_index = 0
 *     ON(1)  → device_index = 1
 */

/* DIP 스위치 읽어서 g_gw_config 적용
 * 반환값: 1=스위치 사용됨, 0=NVS 그대로 사용 */
int dip_switch_apply(void);

#endif /* DIP_SWITCH_H */
