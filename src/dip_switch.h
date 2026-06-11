#ifndef DIP_SWITCH_H
#define DIP_SWITCH_H

#include <stdint.h>

/*
 * DIP 스위치 동작 규칙 (SW1~SW8 전부 장치코드):
 *   SW1~SW8: 8비트 이진수 → 장치코드 인덱스 (0~255)
 *     0 (전부 OFF)  → NVS 저장값 사용 (device_index 포함)
 *     1~150        → dip_code_table[] 에서 장치코드 선택
 *     151~255      → 테이블 미등록 → NVS 저장값 사용
 *
 *   device_index는 딥스위치로 변경하지 않음 — 항상 NVS 저장값 유지
 *
 *   장치코드 적용 시 NVS에도 저장(재부팅 후 유지).
 */

/* DIP 스위치 읽어서 g_gw_config 적용 후 NVS 저장
 * 반환값: 1=스위치 사용됨(NVS 갱신), 0=NVS 그대로 사용 */
int dip_switch_apply(void);

#endif /* DIP_SWITCH_H */
