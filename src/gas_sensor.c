/* 가스 센서 파서
 *
 * 센서 모델은 NVS(g_gw_config.sensor_model_usb*)에 저장되고
 * gas_sensor_start()에서 읽어 포트별 파서를 결정한다.
 *
 * ASCII 센서 (38400 baud):
 *   O2-SM30  : "XX.XX%\r\n"      (8B,  % 종료)
 *   H2S/CO   : "F.F ppm\r\n"     (ppm, float)
 *   CH4-S3   : "F.F ppm\r\n"     (ppm, float, 포맷 확인 필요)
 *   S-300-3V : "DDDDDD ppm\r\n"  (12B, ppm 종료, 6자리)
 *
 * 바이너리 센서 (9600 baud):
 *   AM1002   : 0x16 + 22B 고정 프레임
 *              DF1-2: TVOC(ppb), DF5-6: PM1.0, DF7-8: PM2.5, DF9-10: PM10 (μg/m³)
 *              DF11-12: 온도(raw-500)/10, DF13-14: 습도 raw/10
 */

#include "gas_sensor.h"
#include "config_nvs.h"
#include "ftdi_usb.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(gas_sensor, LOG_LEVEL_INF);

static const char * const s_model_name[GAS_MODEL_COUNT] = {
	"없음",       /* 0 NONE     */
	"O2-SM30",    /* 1 O2       */
	"H2S-SM30",   /* 2 H2S      */
	"CO-SM30",    /* 3 CO       */
	"CH4-S3-3V",  /* 4 CH4      */
	"S-300-3V",   /* 5 CO2 NDIR */
	"AM1002",     /* 6 복합 공기질 */
};

static const char * const s_model_unit[GAS_MODEL_COUNT] = {
	"",       /* 0 NONE  */
	"%Vol",   /* 1 O2    */
	"ppm",    /* 2 H2S   */
	"ppm",    /* 3 CO    */
	"ppm",    /* 4 CH4   */
	"ppm",    /* 5 CO2   */
	"ug/m3",  /* 6 PM2.5 */
};

/* AM1002 22바이트 프레임 + 여유 */
#define ACC_BUF_SIZE 24U

struct sensor_ctx {
	uint8_t            acc_buf[ACC_BUF_SIZE];
	uint8_t            acc_pos;
	gas_reading_t      reading;
	gas_sensor_model_t model;
	struct k_mutex     mutex;
};

static struct sensor_ctx s_sensors[GAS_PORT_COUNT];

/* ── 공개: 모델 이름/단위 ──────────────────────────────────────── */
const char *gas_model_name(gas_sensor_model_t model)
{
	return (model < GAS_MODEL_COUNT) ? s_model_name[model] : "?";
}

const char *gas_model_unit(gas_sensor_model_t model)
{
	return (model < GAS_MODEL_COUNT) ? s_model_unit[model] : "";
}

/* ── 공개: 런타임 모델 변경 ────────────────────────────────────── */
void gas_sensor_set_model(gas_port_t port, gas_sensor_model_t model)
{
	if (port >= GAS_PORT_COUNT || model >= GAS_MODEL_COUNT) {
		return;
	}
	k_mutex_lock(&s_sensors[port].mutex, K_FOREVER);
	s_sensors[port].model   = model;
	s_sensors[port].acc_pos = 0;
	memset(&s_sensors[port].reading, 0, sizeof(gas_reading_t));
	k_mutex_unlock(&s_sensors[port].mutex);

	LOG_INF("USB%d 모델: %s", port == GAS_PORT_USB1 ? 1 : 0,
		gas_model_name(model));
}

gas_sensor_model_t gas_sensor_get_model(gas_port_t port)
{
	return (port < GAS_PORT_COUNT) ? s_sensors[port].model : GAS_MODEL_NONE;
}

/* ── ASCII float: "20.70" → 20.70f ────────────────────────────── */
static float ascii_to_float(const char *s, int len)
{
	float integer = 0.0f;
	float frac    = 0.0f;
	float div     = 1.0f;
	bool  in_frac = false;

	for (int i = 0; i < len; i++) {
		char c = s[i];

		if (c >= '0' && c <= '9') {
			if (!in_frac) {
				integer = integer * 10.0f + (float)(c - '0');
			} else {
				div  *= 10.0f;
				frac += (float)(c - '0') / div;
			}
		} else if (c == '.') {
			in_frac = true;
		}
	}
	return integer + frac;
}

/* ── O2 파서: "XX.XX%\r\n" or "XX.XX %\r\n" ──────────────────── */
static void parse_o2(struct sensor_ctx *sc, uint8_t pos)
{
	int pct_idx = -1;

	if (pos >= 4 && sc->acc_buf[pos - 3] == '%') {
		pct_idx = pos - 3;
	} else if (pos >= 3 && sc->acc_buf[pos - 2] == '%') {
		pct_idx = pos - 2;
	}

	if (pct_idx < 3) {
		LOG_WRN("O2: 동기화 오류 pos=%u", pos);
		return;
	}

	int num_len = (sc->acc_buf[pct_idx - 1] == ' ') ? pct_idx - 1 : pct_idx;

	if (num_len < 2 || num_len > 7) {
		LOG_WRN("O2: 길이 오류 num_len=%d", num_len);
		return;
	}

	char num_str[8] = {0};

	memcpy(num_str, sc->acc_buf, (size_t)num_len);
	float pct = ascii_to_float(num_str, num_len);

	k_mutex_lock(&sc->mutex, K_FOREVER);
	sc->reading.valid = true;
	sc->reading.pct   = pct;
	sc->reading.raw   = (uint16_t)(pct * 100.0f);
	k_mutex_unlock(&sc->mutex);
}

/* ── ppm 파서: "F.F ppm\r\n" / "DDDDDD ppm\r\n" ──────────────── */
static void parse_ppm(struct sensor_ctx *sc, uint8_t pos, gas_sensor_model_t model)
{
	const char *tag = gas_model_name(model);
	int m_idx = -1;

	if (pos >= 4 && sc->acc_buf[pos - 3] == 'm') {
		m_idx = pos - 3;
	} else if (pos >= 3 && sc->acc_buf[pos - 2] == 'm') {
		m_idx = pos - 2;
	}

	if (m_idx < 3 ||
	    sc->acc_buf[m_idx - 1] != 'p' ||
	    sc->acc_buf[m_idx - 2] != 'p') {
		LOG_WRN("%s: 동기화 오류 pos=%u", tag, pos);
		return;
	}

	int num_field_len = m_idx - 3;

	if (num_field_len < 1) {
		LOG_WRN("%s: 숫자 필드 없음", tag);
		return;
	}

	int start = 0;

	while (start < num_field_len && sc->acc_buf[start] == ' ') {
		start++;
	}

	int digit_len = num_field_len - start;

	if (digit_len < 1 || digit_len > 8) {
		LOG_WRN("%s: 자릿수 오류 digit_len=%d", tag, digit_len);
		return;
	}

	char num_str[9] = {0};

	memcpy(num_str, sc->acc_buf + start, (size_t)digit_len);
	float val = ascii_to_float(num_str, digit_len);

	k_mutex_lock(&sc->mutex, K_FOREVER);
	sc->reading.valid = true;
	sc->reading.pct   = val;
	sc->reading.raw   = (uint16_t)(val > 65535.0f ? 65535U : (uint16_t)val);
	k_mutex_unlock(&sc->mutex);
}

/* ── CH4-S3-3V 파서: %LEL(기본) 또는 ppm(옵션) ────────────────── */
/* 기본: "   DD% LEL\r\n" → %LEL 값×500 = ppm 변환 저장           */
/* 옵션: "DDDDDD ppm\r\n" → ppm 직접 저장 (parse_ppm 재사용)       */
static void parse_ch4(struct sensor_ctx *sc, uint8_t pos)
{
	/* % LEL 포맷 감지: ...% LEL\r\n
	 * \r 포함(12B): buf[pos-3]='L', buf[pos-4]='E', buf[pos-5]='L'
	 * \r 없음(11B): buf[pos-2]='L', buf[pos-3]='E', buf[pos-4]='L'
	 */
	int pct_pos = -1;

	if (pos >= 9 &&
	    sc->acc_buf[pos - 3] == 'L' &&
	    sc->acc_buf[pos - 4] == 'E' &&
	    sc->acc_buf[pos - 5] == 'L') {
		pct_pos = pos - 7; /* '%' 위치: pos-7 (공백 1칸 포함) */
	} else if (pos >= 8 &&
		   sc->acc_buf[pos - 2] == 'L' &&
		   sc->acc_buf[pos - 3] == 'E' &&
		   sc->acc_buf[pos - 4] == 'L') {
		pct_pos = pos - 6;
	}

	if (pct_pos >= 0 && pct_pos < pos && sc->acc_buf[pct_pos] == '%') {
		/* % LEL 포맷: 앞 숫자 추출 후 ×500 → ppm */
		int start = 0;

		while (start < pct_pos && sc->acc_buf[start] == ' ') {
			start++;
		}
		int digit_len = pct_pos - start;

		if (digit_len < 1 || digit_len > 3) {
			LOG_WRN("CH4: LEL 자릿수 오류 digit_len=%d", digit_len);
			return;
		}
		char num_str[4] = {0};

		memcpy(num_str, sc->acc_buf + start, (size_t)digit_len);
		float val = ascii_to_float(num_str, digit_len) * 500.0f;

		k_mutex_lock(&sc->mutex, K_FOREVER);
		sc->reading.valid = true;
		sc->reading.pct   = val;
		sc->reading.raw   = (uint16_t)(val > 65535.0f ? 65535U : val);
		k_mutex_unlock(&sc->mutex);
	} else {
		/* ppm 옵션 포맷 */
		parse_ppm(sc, pos, GAS_MODEL_CH4_S3_3V);
	}
}

/* ── AM1002 바이너리 파서: 22바이트 고정 프레임 ────────────────── */
static void parse_am1002(struct sensor_ctx *sc)
{
	const uint8_t *b = sc->acc_buf;

	if (b[0] != 0x16U || b[1] != 0x13U || b[2] != 0x16U) {
		LOG_WRN("AM1002: 헤더 오류 %02X %02X %02X", b[0], b[1], b[2]);
		return;
	}

	/* 체크섬: CS = (256 - sum(byte0..byte20)) & 0xFF */
	uint32_t sum = 0;

	for (int k = 0; k < 21; k++) {
		sum += b[k];
	}
	if ((uint8_t)((256U - (sum & 0xFFU)) & 0xFFU) != b[21]) {
		LOG_WRN("AM1002: 체크섬 오류");
		return;
	}

	uint16_t pm25 = ((uint16_t)b[9] << 8) | b[10];

	k_mutex_lock(&sc->mutex, K_FOREVER);
	sc->reading.valid = true;
	sc->reading.pct   = (float)pm25;
	sc->reading.raw   = pm25;
	k_mutex_unlock(&sc->mutex);
}

/* ── 바이트 스트림 파서 (포트별 모델 분기) ────────────────────── */
static void parse_bytes(gas_port_t port, const uint8_t *data, size_t len)
{
	struct sensor_ctx *sc = &s_sensors[port];
	gas_sensor_model_t model;

	k_mutex_lock(&sc->mutex, K_FOREVER);
	model = sc->model;
	k_mutex_unlock(&sc->mutex);

	for (size_t i = 0; i < len; i++) {
		uint8_t b = data[i];

		if (model == GAS_MODEL_AM1002) {
			/* 바이너리 프레임: 0x16 시작, 22바이트 고정 */
			if (sc->acc_pos == 0U && b != 0x16U) {
				continue; /* 프레임 시작 대기 */
			}
			if (sc->acc_pos < ACC_BUF_SIZE) {
				sc->acc_buf[sc->acc_pos++] = b;
			}
			if (sc->acc_pos >= 22U) {
				parse_am1002(sc);
				sc->acc_pos = 0;
			}
		} else {
			/* ASCII 프레임: LF 종료 */
			if (sc->acc_pos < ACC_BUF_SIZE) {
				sc->acc_buf[sc->acc_pos++] = b;
			}

			if (b == '\n') {
				uint8_t pos = sc->acc_pos;

				switch (model) {
				case GAS_MODEL_O2_SM30:
					parse_o2(sc, pos);
					break;
				case GAS_MODEL_H2S_SM30:
				case GAS_MODEL_CO_SM30:
				case GAS_MODEL_S300_3V:
					parse_ppm(sc, pos, model);
					break;
				case GAS_MODEL_CH4_S3_3V:
					parse_ch4(sc, pos);
					break;
				default:
					/* GAS_MODEL_NONE: 조용히 버림 */
					break;
				}

				sc->acc_pos = 0;
			}

			if (sc->acc_pos >= ACC_BUF_SIZE) {
				LOG_WRN("USB%d: 오버플로우 리셋",
					port == GAS_PORT_USB1 ? 1 : 0);
				sc->acc_pos = 0;
			}
		}
	}
}

/* ── ftdi_usb 수신 콜백 ───────────────────────────────────────── */
static void ftdi_rx_handler(ftdi_port_t port, const uint8_t *data,
			    size_t len, void *user)
{
	ARG_UNUSED(user);

	gas_port_t gport = (port == FTDI_PORT_USB1) ?
			   GAS_PORT_USB1 : GAS_PORT_USB0;

	parse_bytes(gport, data, len);
}

/* ── 공개 API ─────────────────────────────────────────────────── */
void gas_sensor_start(void)
{
	gas_sensor_model_t m1 = (gas_sensor_model_t)g_gw_config.sensor_model_usb1;
	gas_sensor_model_t m0 = (gas_sensor_model_t)g_gw_config.sensor_model_usb0;

	if (m1 >= GAS_MODEL_COUNT) {
		m1 = GAS_MODEL_NONE;
	}
	if (m0 >= GAS_MODEL_COUNT) {
		m0 = GAS_MODEL_NONE;
	}

	for (int i = 0; i < GAS_PORT_COUNT; i++) {
		s_sensors[i].acc_pos = 0;
		memset(&s_sensors[i].reading, 0, sizeof(gas_reading_t));
		k_mutex_init(&s_sensors[i].mutex);
	}
	s_sensors[GAS_PORT_USB1].model = m1;
	s_sensors[GAS_PORT_USB0].model = m0;

	int ret = ftdi_usb_init(FTDI_PORT_USB1, ftdi_rx_handler, NULL);

	if (ret) {
		LOG_ERR("ftdi_usb_init USB1 failed: %d", ret);
	}

	ret = ftdi_usb_init(FTDI_PORT_USB0, ftdi_rx_handler, NULL);
	if (ret) {
		LOG_ERR("ftdi_usb_init USB0 failed: %d", ret);
	}

	LOG_INF("Gas sensors started (USB1=%s USB0=%s)",
		gas_model_name(m1), gas_model_name(m0));
}

gas_reading_t gas_sensor_get(gas_sensor_type_t type)
{
	gas_reading_t r = { .valid = false };

	if (type >= GAS_PORT_COUNT) {
		return r;
	}

	k_mutex_lock(&s_sensors[type].mutex, K_FOREVER);
	r = s_sensors[type].reading;
	k_mutex_unlock(&s_sensors[type].mutex);

	return r;
}

bool gas_sensor_connected(gas_sensor_type_t type)
{
	if (type >= GAS_PORT_COUNT) {
		return false;
	}
	ftdi_port_t port = (type == GAS_PORT_USB1) ?
			   FTDI_PORT_USB1 : FTDI_PORT_USB0;

	return ftdi_usb_connected(port);
}
