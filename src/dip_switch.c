/*
 * DIP 스위치 처리
 *
 * SW1~SW8: 8비트 이진수 → 장치코드 번호 (0=NVS사용, 1~150=코드, 151~255=미등록)
 * device_index: 딥스위치 미사용 — NVS 저장값 유지
 *
 * 장치코드 적용 시 NVS에 저장하여 재부팅 후에도 유지됨.
 * ────────────────────────────────────────────────────────────────
 * 장치코드 테이블: dip_code_table[] 수정 후 리빌드
 * 인덱스 = SW1(bit0) ~ SW8(bit7) 조합값
 * ────────────────────────────────────────────────────────────────
 */

#include "dip_switch.h"
#include "config_nvs.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/devicetree.h>
#include <stdio.h>
#include <string.h>

/* ── 장치 코드 테이블 (여기서 수정) ──────────────────────────────
 * 인덱스 0      : 예약 (SW 전부 OFF → NVS 사용)
 * 인덱스 1~150  : 장치코드
 * 인덱스 151~255: 예약 (NULL → NVS 사용)
 */
static const char * const dip_code_table[256] = {
	NULL,            /* [0]   SW 전부 OFF → NVS 사용 */
	"KRWJARDU001",   /* [1]   */
	"KRWJARDU002",   /* [2]   */
	"KRWJARDU003",   /* [3]   */
	"KRWJARDU004",   /* [4]   */
	"KRWJARDU005",   /* [5]   */
	"KRWJARDU006",   /* [6]   */
	"KRWJARDU007",   /* [7]   */
	"KRWJARDU008",   /* [8]   */
	"KRWJARDU009",   /* [9]   */
	"KRWJARDU010",   /* [10]  */
	"KRWJARDU011",   /* [11]  */
	"KRWJARDU012",   /* [12]  */
	"KRWJARDU013",   /* [13]  */
	"KRWJARDU014",   /* [14]  */
	"KRWJARDU015",   /* [15]  */
	"KRWJARDU016",   /* [16]  */
	"KRWJARDU017",   /* [17]  */
	"KRWJARDU018",   /* [18]  */
	"KRWJARDU019",   /* [19]  */
	"KRWJARDU020",   /* [20]  */
	"KRWJARDU021",   /* [21]  */
	"KRWJARDU022",   /* [22]  */
	"KRWJARDU023",   /* [23]  */
	"KRWJARDU024",   /* [24]  */
	"KRWJARDU025",   /* [25]  */
	"KRWJARDU026",   /* [26]  */
	"KRWJARDU027",   /* [27]  */
	"KRWJARDU028",   /* [28]  */
	"KRWJARDU029",   /* [29]  */
	"KRWJARDU030",   /* [30]  */
	"KRWJARDU031",   /* [31]  */
	"KRWJARDU032",   /* [32]  */
	"KRWJARDU033",   /* [33]  */
	"KRWJARDU034",   /* [34]  */
	"KRWJARDU035",   /* [35]  */
	"KRWJARDU036",   /* [36]  */
	"KRWJARDU037",   /* [37]  */
	"KRWJARDU038",   /* [38]  */
	"KRWJARDU039",   /* [39]  */
	"KRWJARDU040",   /* [40]  */
	"KRWJARDU041",   /* [41]  */
	"KRWJARDU042",   /* [42]  */
	"KRWJARDU043",   /* [43]  */
	"KRWJARDU044",   /* [44]  */
	"KRWJARDU045",   /* [45]  */
	"KRWJARDU046",   /* [46]  */
	"KRWJARDU047",   /* [47]  */
	"KRWJARDU048",   /* [48]  */
	"KRWJARDU049",   /* [49]  */
	"KRWJARDU050",   /* [50]  */
	"KRWJARDU051",   /* [51]  */
	"KRWJARDU052",   /* [52]  */
	"KRWJARDU053",   /* [53]  */
	"KRWJARDU054",   /* [54]  */
	"KRWJARDU055",   /* [55]  */
	"KRWJARDU056",   /* [56]  */
	"KRWJARDU057",   /* [57]  */
	"KRWJARDU058",   /* [58]  */
	"KRWJARDU059",   /* [59]  */
	"KRWJARDU060",   /* [60]  */
	"KRWJARDU061",   /* [61]  */
	"KRWJARDU062",   /* [62]  */
	"KRWJARDU063",   /* [63]  */
	"KRWJARDU064",   /* [64]  */
	"KRWJARDU065",   /* [65]  */
	"KRWJARDU066",   /* [66]  */
	"KRWJARDU067",   /* [67]  */
	"KRWJARDU068",   /* [68]  */
	"KRWJARDU069",   /* [69]  */
	"KRWJARDU070",   /* [70]  */
	"KRWJARDU071",   /* [71]  */
	"KRWJARDU072",   /* [72]  */
	"KRWJARDU073",   /* [73]  */
	"KRWJARDU074",   /* [74]  */
	"KRWJARDU075",   /* [75]  */
	"KRWJARDU076",   /* [76]  */
	"KRWJARDU077",   /* [77]  */
	"KRWJARDU078",   /* [78]  */
	"KRWJARDU079",   /* [79]  */
	"KRWJARDU080",   /* [80]  */
	"KRWJARDU081",   /* [81]  */
	"KRWJARDU082",   /* [82]  */
	"KRWJARDU083",   /* [83]  */
	"KRWJARDU084",   /* [84]  */
	"KRWJARDU085",   /* [85]  */
	"KRWJARDU086",   /* [86]  */
	"KRWJARDU087",   /* [87]  */
	"KRWJARDU088",   /* [88]  */
	"KRWJARDU089",   /* [89]  */
	"KRWJARDU090",   /* [90]  */
	"KRWJARDU091",   /* [91]  */
	"KRWJARDU092",   /* [92]  */
	"KRWJARDU093",   /* [93]  */
	"KRWJARDU094",   /* [94]  */
	"KRWJARDU095",   /* [95]  */
	"KRWJARDU096",   /* [96]  */
	"KRWJARDU097",   /* [97]  */
	"KRWJARDU098",   /* [98]  */
	"KRWJARDU099",   /* [99]  */
	"KRWJARDU100",   /* [100] */
	"KRWJARDU101",   /* [101] */
	"KRWJARDU102",   /* [102] */
	"KRWJARDU103",   /* [103] */
	"KRWJARDU104",   /* [104] */
	"KRWJARDU105",   /* [105] */
	"KRWJARDU106",   /* [106] */
	"KRWJARDU107",   /* [107] */
	"KRWJARDU108",   /* [108] */
	"KRWJARDU109",   /* [109] */
	"KRWJARDU110",   /* [110] */
	"KRWJARDU111",   /* [111] */
	"KRWJARDU112",   /* [112] */
	"KRWJARDU113",   /* [113] */
	"KRWJARDU114",   /* [114] */
	"KRWJARDU115",   /* [115] */
	"KRWJARDU116",   /* [116] */
	"KRWJARDU117",   /* [117] */
	"KRWJARDU118",   /* [118] */
	"KRWJARDU119",   /* [119] */
	"KRWJARDU120",   /* [120] */
	"KRWJARDU121",   /* [121] */
	"KRWJARDU122",   /* [122] */
	"KRWJARDU123",   /* [123] */
	"KRWJARDU124",   /* [124] */
	"KRWJARDU125",   /* [125] */
	"KRWJARDU126",   /* [126] */
	"KRWJARDU127",   /* [127] */
	"KRWJARDU128",   /* [128] */
	"KRWJARDU129",   /* [129] */
	"KRWJARDU130",   /* [130] */
	"KRWJARDU131",   /* [131] */
	"KRWJARDU132",   /* [132] */
	"KRWJARDU133",   /* [133] */
	"KRWJARDU134",   /* [134] */
	"KRWJARDU135",   /* [135] */
	"KRWJARDU136",   /* [136] */
	"KRWJARDU137",   /* [137] */
	"KRWJARDU138",   /* [138] */
	"KRWJARDU139",   /* [139] */
	"KRWJARDU140",   /* [140] */
	"KRWJARDU141",   /* [141] */
	"KRWJARDU142",   /* [142] */
	"KRWJARDU143",   /* [143] */
	"KRWJARDU144",   /* [144] */
	"KRWJARDU145",   /* [145] */
	"KRWJARDU146",   /* [146] */
	"KRWJARDU147",   /* [147] */
	"KRWJARDU148",   /* [148] */
	"KRWJARDU149",   /* [149] */
	"KRWJARDU150",   /* [150] */
	/* [151~255]: 미등록 → NULL → NVS 사용 */
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
	GPIO_DT_SPEC_GET_BY_IDX(DIP_SW_NODE, dip_sw_gpios, 7), /* SW8 bit7 */
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

	/* SW1~SW8 읽어서 8비트 값 계산 */
	uint8_t code_idx = 0;
	int sw_state[8];

	for (int i = 0; i < 8; i++) {
		sw_state[i] = gpio_pin_get_dt(&dip_sw[i]) > 0 ? 1 : 0;
		if (sw_state[i]) {
			code_idx |= (uint8_t)(1U << i);
		}
	}

	/* ── DIP 스위치 상태 로그 ── */
	printf("[DIP] ┌─ 스위치 상태 ─────────────────────────────────────┐\n");
	printf("[DIP] │ SW1=%d SW2=%d SW3=%d SW4=%d SW5=%d SW6=%d SW7=%d SW8=%d │ 값=%u(0x%02X)\n",
	       sw_state[0], sw_state[1], sw_state[2], sw_state[3],
	       sw_state[4], sw_state[5], sw_state[6], sw_state[7],
	       code_idx, code_idx);
	printf("[DIP] └───────────────────────────────────────────────────┘\n");

	/* code_idx=0: 전부 OFF → NVS 사용 */
	if (code_idx == 0) {
		printf("[DIP] → SW 전부 OFF: NVS 저장값 사용\n");
		printf("[DIP]   현재 NVS: 코드=%s  인덱스=%u\n",
		       g_gw_config.master_code, g_gw_config.device_index);
		return 0;
	}

	/* 테이블 미등록 (NULL) → NVS 유지 */
	if (dip_code_table[code_idx] == NULL) {
		printf("[DIP] → 코드값 %u(0x%02X): 테이블 미등록 → NVS 저장값 사용\n",
		       code_idx, code_idx);
		return 0;
	}

	/* g_gw_config 덮어쓰기 + NVS 저장 (device_index는 NVS 유지) */
	strncpy(g_gw_config.master_code, dip_code_table[code_idx],
		sizeof(g_gw_config.master_code) - 1);
	g_gw_config.master_code[sizeof(g_gw_config.master_code) - 1] = '\0';

	printf("[DIP] → 적용: 장치코드=%-12s  인덱스=%u(NVS) → NVS 저장\n",
	       g_gw_config.master_code, g_gw_config.device_index);

	config_nvs_save();

	return 1;
}
