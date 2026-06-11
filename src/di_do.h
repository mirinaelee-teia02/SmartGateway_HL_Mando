/*
 * SmartGateway — DI/DO I2C GPIO 확장기 (PCA9554DB)
 *
 * DO: LPI2C1(FC1) P0_20/P0_21 → PCA9554DB(0x22) 8채널 디지털 출력
 * DI: LPI2C7(FC7) P3_2/P3_3   → PCA9554DB(0x21) 8채널 디지털 입력
 * EXT_PWR1: P0_24, EXT_PWR2: P0_25 (GPIO0 출력)
 * DO_INT:   P3_0,  DI_INT:   P3_1  (GPIO3 입력, 향후 인터럽트 확장)
 */

#ifndef DI_DO_H
#define DI_DO_H

#include <stdint.h>
#include <stdbool.h>

/* 하드웨어 초기화 (main에서 nvs 메뉴 전에 호출) */
int  di_do_init(void);

/* 백그라운드 폴링 태스크 시작 (init 후 호출) */
int  di_do_task_start(void);

/* DO: 8비트 출력 마스크 설정/읽기 (bit0=DO0 … bit7=DO7) */
int     do_set(uint8_t mask);
uint8_t do_get(void);

/* DI: 8비트 입력 읽기 (직접 I2C) / 캐시된 최신 값 반환 */
int     di_read(uint8_t *val);
uint8_t di_get(void);

/* 외부 전원 제어/읽기 */
void ext_pwr1_set(bool on);
void ext_pwr2_set(bool on);
bool ext_pwr1_get(void);
bool ext_pwr2_get(void);

#endif /* DI_DO_H */
