/*
 * DIP 스위치 처리
 *
 * SW1~SW7: 7비트 이진수 → 장치코드 번호 (0=NVS사용, 1~127)
 * SW8    : 장치 인덱스 (OFF=0, ON=1)
 *
 * ────────────────────────────────────────────────
 * 장치코드 테이블: dip_code_table[] 을 직접 수정하세요
 * 인덱스 = SW1(bit0) ~ SW7(bit6) 조합값
 * ────────────────────────────────────────────────
 */

#include "dip_switch.h"
#include "config_nvs.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/devicetree.h>
#include <stdio.h>
#include <string.h>

/* ── 장치 코드 테이블 (여기서 수정) ──────────────────────────────
 * 인덱스 0   : 예약 (SW 전부 OFF → NVS 사용)
 * 인덱스 1   : SW1 ON
 * 인덱스 2   : SW2 ON
 * 인덱스 3   : SW1+SW2 ON
 * ...최대 127
 */
static const char * const dip_code_table[128] = {
	NULL,          /* [0]  SW 전부 OFF → NVS 사용 */
	"KRWJARDU001", /* [1]  */
	"KRWJARDU002", /* [2]  */
	"KRWJARDU003", /* [3]  */
	"KRWJARDU004", /* [4]  */
	"KRWJARDU005", /* [5]  */
	"KRWJARDU006", /* [6]  */
	"KRWJARDU007", /* [7]  */
	"KRWJARDU008", /* [8]  */
	"KRWJARDU009", /* [9]  */
	"KRWJARDU010", /* [10] */
	"KRWJARDU011", /* [11] */
	"KRWJARDU012", /* [12] */
	"KRWJARDU013", /* [13] */
	"KRWJARDU014", /* [14] */
	"KRWJARDU015", /* [15] */
	/* 필요에 따라 추가 */
};

/* ── DIP 스위치 GPIO 스펙 (overlay의 dip-sw-gpios 순서대로) ── */
#define DIP_SW_NODE DT_PATH(zephyr_user)

static const struct gpio_dt_spec dip_sw[8] = {
	GPIO_DT_SPEC_GET_BY_IDX(DIP_SW_NODE, dip_sw_gpios, 0), /* SW1 bit0 */
	GPIO_DT_SPEC_GET_BY_IDX(DIP_SW_NODE, dip_sw_gpios, 1), /* SW2 bit1 */
	GPIO_DT_SPEC_GET_BY_IDX(DIP_SW_NODE, dip_sw_gpios, 2), /* SW3 bit2 */
	GPIO_DT_SPEC_GET_BY_IDX(DIP_SW_NODE, dip_sw_gpios, 3), /* SW4 bit3 */
	GPIO_DT_SPEC_GET_BY_IDX(DIP_SW_NODE, dip_sw_gpios, 4), /* SW5 bit4 */
	GPIO_DT_SPEC_GET_BY_IDX(DIP_SW_NODE, dip_sw_gpios, 5), /* SW6 bit5 */
	GPIO_DT_SPEC_GET_BY_IDX(DIP_SW_NODE, dip_sw_gpios, 6), /* SW7 bit6 */
	GPIO_DT_SPEC_GET_BY_IDX(DIP_SW_NODE, dip_sw_gpios, 7), /* SW8 인덱스 */
};

int dip_switch_apply(void)
{
	/* GPIO 초기화 */
	for (int i = 0; i < 8; i++) {
		if (!gpio_is_ready_dt(&dip_sw[i])) {
			printf("[DIP] GPIO SW%d not ready\n", i + 1);
			return 0;
		}
		gpio_pin_configure_dt(&dip_sw[i], GPIO_INPUT);
	}

	/* SW1~SW7 읽어서 7비트 값 계산 */
	uint8_t code_idx = 0;
	int sw_state[8];

	for (int i = 0; i < 7; i++) {
		sw_state[i] = gpio_pin_get_dt(&dip_sw[i]) > 0 ? 1 : 0;
		if (sw_state[i]) {
			code_idx |= (uint8_t)(1U << i);
		}
	}

	/* SW8 읽기 (인덱스) */
	sw_state[7] = gpio_pin_get_dt(&dip_sw[7]) > 0 ? 1 : 0;
	uint16_t dev_index = (uint16_t)sw_state[7];

	/* ── DIP 스위치 상태 로그 ── */
	printf("[DIP] ┌─ 스위치 상태 ─────────────────────────────┐\n");
	printf("[DIP] │ SW1=%d SW2=%d SW3=%d SW4=%d SW5=%d SW6=%d SW7=%d │ 코드값=%u(0x%02X)\n",
	       sw_state[0], sw_state[1], sw_state[2], sw_state[3],
	       sw_state[4], sw_state[5], sw_state[6],
	       code_idx, code_idx);
	printf("[DIP] │ SW8=%d (인덱스=%u)                          │\n",
	       sw_state[7], dev_index);
	printf("[DIP] └───────────────────────────────────────────┘\n");

	/* code_idx=0: 전부 OFF → NVS 사용 */
	if (code_idx == 0) {
		printf("[DIP] → SW1~7 전부 OFF: NVS 저장값 사용\n");
		printf("[DIP]   현재 NVS: 코드=%s  인덱스=%u\n",
		       g_gw_config.master_code, g_gw_config.device_index);
		return 0;
	}

	/* 테이블 범위 초과 또는 NULL → 로그 후 NVS 유지 */
	if (code_idx >= ARRAY_SIZE(dip_code_table) ||
	    dip_code_table[code_idx] == NULL) {
		printf("[DIP] → 코드값 %u: 테이블 미등록 → NVS 저장값 사용\n", code_idx);
		return 0;
	}

	/* g_gw_config 덮어쓰기 (NVS 무시) */
	strncpy(g_gw_config.master_code, dip_code_table[code_idx],
		sizeof(g_gw_config.master_code) - 1);
	g_gw_config.master_code[sizeof(g_gw_config.master_code) - 1] = '\0';
	g_gw_config.device_index = dev_index;

	printf("[DIP] → 적용: 장치코드=%-12s  인덱스=%u  (NVS 무시)\n",
	       g_gw_config.master_code, dev_index);
	return 1;
}
