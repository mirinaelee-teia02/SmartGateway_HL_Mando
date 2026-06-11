/* 가스 센서 인터페이스
 *
 * USB1(EHCI) → FT2232 → 센서 (모델: NVS 메뉴 T에서 포트별 선택)
 * USB0(KHCI) → FT2232 → 센서 (모델: NVS 메뉴 T에서 포트별 선택)
 *
 * ※ AM1002는 9600 baud / 바이너리 프로토콜 — 나머지는 38400 baud / ASCII
 */

#ifndef GAS_SENSOR_H
#define GAS_SENSOR_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 센서 모델 — NVS 메뉴 T에서 포트별 선택 */
typedef enum {
	GAS_MODEL_NONE     = 0, /* 미설정 — 파싱 안 함 */
	GAS_MODEL_O2_SM30  = 1, /* O2-SM30-3V   "XX.XX%\r\n"     %Vol   1s  38400 */
	GAS_MODEL_H2S_SM30 = 2, /* H2S-SM30-3V  "F.F ppm\r\n"    ppm    1s  38400 */
	GAS_MODEL_CO_SM30  = 3, /* CO-SM30-3V   "F.F ppm\r\n"    ppm    1s  38400 */
	GAS_MODEL_CH4_S3_3V = 4, /* CH4-S3-3V "   DD% LEL\r\n"(기본) / "DDDDDD ppm\r\n"(옵션) 3s 38400 */
	GAS_MODEL_S300_3V  = 5, /* S-300-3V CO2 "DDDDDD ppm\r\n" ppm    3s  38400 */
	GAS_MODEL_AM1002   = 6, /* AM1002 복합  바이너리 22B    PM2.5  ~1s  9600  */
	GAS_MODEL_COUNT
} gas_sensor_model_t;

/* USB 포트 인덱스 */
typedef enum {
	GAS_PORT_USB1 = 0, /* EHCI HS */
	GAS_PORT_USB0 = 1, /* KHCI FS */
	GAS_PORT_COUNT
} gas_port_t;

/* 하위 호환 alias */
#define GAS_SENSOR_O2    GAS_PORT_USB1
#define GAS_SENSOR_H2S   GAS_PORT_USB0
#define GAS_SENSOR_COUNT GAS_PORT_COUNT
typedef gas_port_t gas_sensor_type_t;

typedef struct {
	bool     valid;
	/* O2: %Vol×100; ppm 센서: ppm; AM1002: PM2.5 μg/m³ */
	uint16_t raw;
	/* O2: %Vol; ppm 센서: ppm float; AM1002: PM2.5 float */
	float    pct;
} gas_reading_t;

/** USB HOST + FT2232 두 포트 모두 시작. g_gw_config에서 모델 읽음 */
void gas_sensor_start(void);

/** 최근 측정값 반환 (valid=false → 아직 데이터 없음) */
gas_reading_t gas_sensor_get(gas_sensor_type_t type);

/** FT2232 연결 여부 */
bool gas_sensor_connected(gas_sensor_type_t type);

/** 런타임 모델 변경 (NVS 메뉴 T에서 호출) — 버퍼/측정값 초기화 포함 */
void gas_sensor_set_model(gas_port_t port, gas_sensor_model_t model);
gas_sensor_model_t gas_sensor_get_model(gas_port_t port);

/** 표시용 이름 / 단위 문자열 */
const char *gas_model_name(gas_sensor_model_t model);
const char *gas_model_unit(gas_sensor_model_t model);

/* 하위 호환 */
static inline gas_reading_t gas_sensor_get_o2(void)
{
	return gas_sensor_get(GAS_SENSOR_O2);
}

#ifdef __cplusplus
}
#endif

#endif /* GAS_SENSOR_H */
