/*
 * Smart Gateway - MessagePack
 *
 * 운영(map): line, timestamp(14자), raw/min/max
 * 테스트(array): [ device_str, timestamp(17자), float32, float32 ]
 */

#include "msgpack_adc.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <zephyr/sys/byteorder.h>

#define MSGPACK_FIXMAP(n)    (0x80U | ((n) & 0x0f))
#define MSGPACK_FIXARRAY(n)  (0x90U | ((n) & 0x0f))
#define MSGPACK_FLOAT32      0xca
#define MSGPACK_FIXSTR(n)    (0xa0U | ((n) & 0x1f))
#define MSGPACK_STR8         0xd9

static inline void write_float32_be(uint8_t *p, float f)
{
	uint32_t u;

	memcpy(&u, &f, sizeof(u));
	u = sys_cpu_to_be32(u);
	memcpy(p, &u, 4);
}

/* UDP float32: 소수 둘째 자리로 반올림 후 인코딩 (표시/파서 일치용) */
static inline float float32_round_2decimals(float x)
{
	double d = (double)x * 100.0;
	long n;

	if (d >= 0.0) {
		n = (long)(d + 0.5);
	} else {
		n = (long)(d - 0.5);
	}
	return (float)((double)n / 100.0);
}

static int encode_map_kv(uint8_t **pp, size_t *remain, const char *key, size_t klen,
			 const char *val, size_t vlen)
{
	uint8_t *p = *pp;
	size_t r = *remain;
	size_t need = 1 + klen;

	if (klen > 31 || vlen > 255) {
		return -1;
	}
	if (vlen <= 31) {
		need += 1 + vlen;
	} else {
		need += 2 + vlen;
	}
	if (r < need) {
		return -1;
	}

	*p++ = MSGPACK_FIXSTR((uint8_t)klen);
	memcpy(p, key, klen);
	p += klen;

	if (vlen <= 31) {
		*p++ = MSGPACK_FIXSTR((uint8_t)vlen);
	} else {
		*p++ = MSGPACK_STR8;
		*p++ = (uint8_t)vlen;
	}
	memcpy(p, val, vlen);
	p += vlen;

	*pp = p;
	*remain = r - need;
	return 0;
}

static int encode_map_str(uint8_t **pp, size_t *remain, const char *key, const char *val)
{
	return encode_map_kv(pp, remain, key, strlen(key), val, strlen(val));
}

static size_t line_or_device_id_len(const char *val)
{
	size_t n = 0;

	while (n < ADC_LINE_MAX_LEN && val[n] != '\0') {
		n++;
	}
	return (n > ADC_LINE_ID_MAX_CHARS) ? ADC_LINE_ID_MAX_CHARS : n;
}

/* line / DeviveID: 최대 ADC_LINE_ID_MAX_CHARS */
static int encode_map_line_or_device_id(uint8_t **pp, size_t *remain, const char *key,
					const char *val)
{
	return encode_map_kv(pp, remain, key, strlen(key), val, line_or_device_id_len(val));
}

/* yyyyMMddHHmmss — 14자, 밀리초 없음 (운영 map용) */
static int fmt_timestamp_str(char *out, size_t out_sz, const adc_snapshot_t *snap)
{
	int n = snprintf(out, out_sz, "%04u%02u%02u%02u%02u%02u",
			 (unsigned)snap->datetime.year, (unsigned)snap->datetime.month,
			 (unsigned)snap->datetime.day, (unsigned)snap->datetime.hour,
			 (unsigned)snap->datetime.min, (unsigned)snap->datetime.sec);

	if (n != 14 || (size_t)n >= out_sz) {
		return -1;
	}
	return 0;
}

/* yyyyMMddHHmmssfff — 17자 (테스트 array 2번째 요소) */
static int fmt_timestamp_str_test(char *out, size_t out_sz, const adc_snapshot_t *snap)
{
	int n = snprintf(out, out_sz, "%04u%02u%02u%02u%02u%02u%03u",
			 (unsigned)snap->datetime.year, (unsigned)snap->datetime.month,
			 (unsigned)snap->datetime.day, (unsigned)snap->datetime.hour,
			 (unsigned)snap->datetime.min, (unsigned)snap->datetime.sec,
			 (unsigned)snap->msec);

	if (n != 17 || (size_t)n >= out_sz) {
		return -1;
	}
	return 0;
}

/* map 키 없이 MessagePack str 한 개만 출력 (배열 요소용) */
static int append_msgpack_str(uint8_t **pp, size_t *remain, const char *s, size_t slen)
{
	uint8_t *p = *pp;
	size_t r = *remain;
	size_t need;

	if (slen > 255U) {
		return -1;
	}
	if (slen <= 31U) {
		need = 1 + slen;
	} else {
		need = 2 + slen;
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

static int encode_map_array_f32(uint8_t **pp, size_t *remain, const char *key,
				const float *arr, uint8_t n)
{
	uint8_t *p = *pp;
	size_t r = *remain;
	size_t klen = strlen(key);
	size_t need = 1 + klen + 1 + n * 5;

	if (r < need) {
		return -1;
	}

	*p++ = MSGPACK_FIXSTR((uint8_t)klen);
	memcpy(p, key, klen);
	p += klen;
	*p++ = MSGPACK_FIXARRAY(n);
	for (uint8_t i = 0; i < n; i++) {
		*p++ = MSGPACK_FLOAT32;
		write_float32_be(p, float32_round_2decimals(arr[i]));
		p += 4;
	}
	*pp = p;
	*remain = r - need;
	return 0;
}

int msgpack_encode_adc_test_2ch(const adc_snapshot_t *snap, uint8_t *buf, size_t buf_len)
{
	if (!snap || !buf || snap->ch_count < 2) {
		return -1;
	}

	char ts[20];
	size_t id_len = line_or_device_id_len(snap->line);

	if (fmt_timestamp_str_test(ts, sizeof(ts), snap) != 0) {
		return -1;
	}

	uint8_t *p = buf;
	size_t remain = buf_len;
	/* fixarray 4 + str + str + float32 + float32 */
	size_t need = 1U + (id_len <= 31U ? 1U + id_len : 2U + id_len) +
		      (17U <= 31U ? 1U + 17U : 2U + 17U) + 5U + 5U;

	if (remain < need) {
		return -1;
	}

	*p++ = MSGPACK_FIXARRAY(4);
	remain--;

	if (append_msgpack_str(&p, &remain, snap->line, id_len) != 0) {
		return -1;
	}
	if (append_msgpack_str(&p, &remain, ts, 17U) != 0) {
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

int msgpack_encode_adc_snapshot(const adc_snapshot_t *snap, uint8_t *buf, size_t buf_len)
{
	if (!snap || !buf || snap->ch_count == 0 || snap->ch_count > ADC_MAX_CHANNELS) {
		return -1;
	}

	char ts[16];

	if (fmt_timestamp_str(ts, sizeof(ts), snap) != 0) {
		return -1;
	}

	uint8_t *p = buf;
	size_t remain = buf_len;
	uint8_t n = snap->ch_count;

	if (remain < 1) {
		return -1;
	}
	*p++ = MSGPACK_FIXMAP(5);
	remain--;

	if (encode_map_line_or_device_id(&p, &remain, "line", snap->line) != 0) {
		return -1;
	}
	if (encode_map_str(&p, &remain, "timestamp", ts) != 0) {
		return -1;
	}
	if (encode_map_array_f32(&p, &remain, "raw", snap->raw, n) != 0) {
		return -1;
	}
	if (encode_map_array_f32(&p, &remain, "min", snap->min_val, n) != 0) {
		return -1;
	}
	if (encode_map_array_f32(&p, &remain, "max", snap->max_val, n) != 0) {
		return -1;
	}

	return (int)(p - buf);
}
