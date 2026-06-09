/*
 * Smart Gateway - MessagePack UDP 페이로드
 *
 * 운영(UDP): DC 00 1C + 28필드 순차 (DeviceID, Timestamp, V×8, min×8, max×8, Sampling, ERROR)
 * 테스트: fixarray 4 — [ device, timestamp(17), ch0, ch1 ]
 */

#include "msgpack_adc.h"
#include "gw_error.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <zephyr/sys/byteorder.h>

#define MSGPACK_FIXARRAY(n)  (0x90U | ((n) & 0x0f))
#define MSGPACK_FIXSTR(n)      (0xa0U | ((n) & 0x1f))
#define MSGPACK_FLOAT32        0xcaU
#define MSGPACK_UINT8          0xccU
#define MSGPACK_STR8           0xd9U
/* 규격 헤더: 0xDC 0x00 0x1C — 필드 개수 28 (MessagePack map16 형식 코드) */
#define UDP_MSGPACK_HDR0       0xdcU
#define UDP_MSGPACK_HDR_CNT_HI 0x00U
#define UDP_MSGPACK_HDR_CNT_LO 0x1cU
#define UDP_MSGPACK_FIELD_CNT  28U
#define UDP_MSGPACK_CH_FIELDS  8U
#define UDP_TS_WIRE_LEN        17U

static inline void write_float32_be(uint8_t *p, float f)
{
	uint32_t u;

	memcpy(&u, &f, sizeof(u));
	u = sys_cpu_to_be32(u);
	memcpy(p, &u, 4);
}

static inline float float32_round_2decimals(float x)
{
	float d = x * 100.0f;
	long n;

	if (d >= 0.0f) {
		n = (long)(d + 0.5f);
	} else {
		n = (long)(d - 0.5f);
	}
	return (float)n / 100.0f;
}

static size_t device_id_wire_len(const char *val)
{
	size_t n = 0;

	while (n < ADC_LINE_MAX_LEN && val[n] != '\0') {
		n++;
	}
	if (n > ADC_LINE_ID_MAX_CHARS) {
		n = ADC_LINE_ID_MAX_CHARS;
	}
	if (n > 255U) {
		n = 255U;
	}
	return n;
}

static int fmt_timestamp_udp17(char *out, size_t out_sz, const adc_snapshot_t *snap)
{
	int n = snprintf(out, out_sz, "%04u%02u%02u%02u%02u%02u%03u",
			 (unsigned)snap->datetime.year, (unsigned)snap->datetime.month,
			 (unsigned)snap->datetime.day, (unsigned)snap->datetime.hour,
			 (unsigned)snap->datetime.min, (unsigned)snap->datetime.sec,
			 (unsigned)snap->msec);

	if (n != (int)UDP_TS_WIRE_LEN || (size_t)n >= out_sz) {
		return -1;
	}
	return 0;
}

static int append_fixstr(uint8_t **pp, size_t *remain, const char *s, size_t slen)
{
	uint8_t *p = *pp;
	size_t r = *remain;
	size_t need;

	if (slen > 255U) {
		return -1;
	}
	if (slen <= 31U) {
		need = 1U + slen;
	} else {
		need = 2U + slen;
	}
	if (r < need) {
		return -1;
	}
	if (slen <= 31U) {
		*p++ = MSGPACK_FIXSTR((uint8_t)slen);
	} else {
		*p++ = MSGPACK_STR8;
		*p++ = (uint8_t)slen;
	}
	memcpy(p, s, slen);
	p += slen;
	*pp = p;
	*remain = r - need;
	return 0;
}

static int append_float32(uint8_t **pp, size_t *remain, float v)
{
	uint8_t *p = *pp;

	if (*remain < 5U) {
		return -1;
	}
	*p++ = MSGPACK_FLOAT32;
	write_float32_be(p, float32_round_2decimals(v));
	p += 4U;
	*pp = p;
	*remain -= 5U;
	return 0;
}

static int append_uint8(uint8_t **pp, size_t *remain, uint8_t v)
{
	uint8_t *p = *pp;

	if (*remain < 2U) {
		return -1;
	}
	*p++ = MSGPACK_UINT8;
	*p++ = v;
	*pp = p;
	*remain -= 2U;
	return 0;
}

static float channel_volts(const adc_snapshot_t *snap, const float *arr, uint8_t ch)
{
	if (ch < snap->ch_count) {
		return arr[ch];
	}
	return 0.0f;
}

int msgpack_encode_adc_snapshot(const adc_snapshot_t *snap, uint8_t *buf, size_t buf_len)
{
	char ts[20];
	size_t id_len;
	uint8_t sampling_u8;
	uint8_t err_code;

	if (!snap || !buf || snap->ch_count == 0 || snap->ch_count > ADC_MAX_CHANNELS) {
		return -1;
	}

	id_len = device_id_wire_len(snap->line);
	if (fmt_timestamp_udp17(ts, sizeof(ts), snap) != 0) {
		return -1;
	}

	sampling_u8 = (snap->sample_count > 255U) ? 255U : (uint8_t)snap->sample_count;
	err_code = gw_error_get();

	/* 헤더 3 + str + str + 24×float + 2×uint8 */
	size_t need = 3U + (id_len <= 31U ? 1U + id_len : 2U + id_len) +
		      (1U + UDP_TS_WIRE_LEN) + (24U * 5U) + (2U * 2U);

	if (buf_len < need) {
		return -1;
	}

	uint8_t *p = buf;
	size_t remain = buf_len;

	*p++ = UDP_MSGPACK_HDR0;
	*p++ = UDP_MSGPACK_HDR_CNT_HI;
	*p++ = UDP_MSGPACK_HDR_CNT_LO;
	remain -= 3U;

	if (append_fixstr(&p, &remain, snap->line, id_len) != 0) {
		return -1;
	}
	if (append_fixstr(&p, &remain, ts, UDP_TS_WIRE_LEN) != 0) {
		return -1;
	}

	for (uint8_t i = 0; i < UDP_MSGPACK_CH_FIELDS; i++) {
		if (append_float32(&p, &remain, channel_volts(snap, snap->raw, i)) != 0) {
			return -1;
		}
	}
	for (uint8_t i = 0; i < UDP_MSGPACK_CH_FIELDS; i++) {
		if (append_float32(&p, &remain, channel_volts(snap, snap->min_val, i)) != 0) {
			return -1;
		}
	}
	for (uint8_t i = 0; i < UDP_MSGPACK_CH_FIELDS; i++) {
		if (append_float32(&p, &remain, channel_volts(snap, snap->max_val, i)) != 0) {
			return -1;
		}
	}
	if (append_uint8(&p, &remain, sampling_u8) != 0) {
		return -1;
	}
	if (append_uint8(&p, &remain, err_code) != 0) {
		return -1;
	}

	ARG_UNUSED(UDP_MSGPACK_FIELD_CNT);
	return (int)(p - buf);
}

/* ── 테스트 모드: fixarray 4 (2ch) ───────────────────────────────────── */

int msgpack_encode_adc_test_2ch(const adc_snapshot_t *snap, uint8_t *buf, size_t buf_len)
{
	if (!snap || !buf || snap->ch_count < 2) {
		return -1;
	}

	char ts[20];
	size_t id_len = device_id_wire_len(snap->line);

	if (fmt_timestamp_udp17(ts, sizeof(ts), snap) != 0) {
		return -1;
	}

	uint8_t *p = buf;
	size_t remain = buf_len;
	size_t need = 1U + (id_len <= 31U ? 1U + id_len : 2U + id_len) +
		      (17U <= 31U ? 1U + 17U : 2U + 17U) + 5U + 5U;

	if (remain < need) {
		return -1;
	}

	*p++ = MSGPACK_FIXARRAY(4);
	remain--;

	if (append_fixstr(&p, &remain, snap->line, id_len) != 0) {
		return -1;
	}
	if (append_fixstr(&p, &remain, ts, 17U) != 0) {
		return -1;
	}
	if (remain < 10U) {
		return -1;
	}
	*p++ = MSGPACK_FLOAT32;
	write_float32_be(p, float32_round_2decimals(snap->raw[0]));
	p += 4;
	*p++ = MSGPACK_FLOAT32;
	write_float32_be(p, float32_round_2decimals(snap->raw[1]));
	p += 4;

	return (int)(p - buf);
}
