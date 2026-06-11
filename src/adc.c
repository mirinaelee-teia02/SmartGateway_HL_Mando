/*
 * Smart Gateway - ADC 모듈 구현 (AD7327 SPI 8채널)
 *
 * - k_timer 2ms 주기로 샘플 트리거
 * - AD7327: 12-bit, ±10V (±2VREF, 내부VREF=2.5V), two's complement
 *   ADC 입력단 하드웨어 분배기 2:1 → 실제 입력범위 ±10V
 * - LPSPI6(FC6): P3_20=MOSI, P3_21=SCK, P3_22=MISO, P3_23=PCS0
 * - 2초 윈도우 min/max, UDP 20ms 전송
 *
 * AD7327 SPI 프로토콜:
 *   - 16bit MSB-first, Mode 0 (CPOL=0, CPHA=0)
 *   - 제어어 쓰기 → CS 해제(변환 시작) → 다음 전송 시 결과 읽기 (read-ahead)
 *   - 8채널 읽기: 9회 전송 (첫 결과 폐기)
 *
 * 제어어 형식:
 *   [15]=1(REG_SEL) [14:12]=ADD[2:0] [11:10]=RANGE(01=±2VREF)
 *   [9]=CODING(1=2's comp) [8]=REF_SEL(0=ext) [7:0]=0
 *
 * 결과어 형식:
 *   [15:13]=CH[2:0] [12:1]=12bit 데이터 [0]=0
 */

#include "adc.h"
#include "config_nvs.h"
#include "time_helper.h"

#include <stdio.h>
#include <string.h>
#include <zephyr/autoconf.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/byteorder.h>

/* ── AD7327 상수 ────────────────────────────────────────────── */
#define AD7327_NUM_CHANNELS	8
#define AD7327_RESOLUTION	12
#define AD7327_FULL_SCALE	(1 << (AD7327_RESOLUTION - 1))	/* 2048 */
#define AD7327_VREF_V		10.0f	/* ±10V 

/* AD7327 Control Register 1 (REG_SEL=00) 비트 구성:
 * bit15    = WRITE=1
 * bit14:13 = REG_SEL=00 (Control Register 1 선택)
 * bit12:10 = ADD[2:0]   = 채널번호
 * bit9:8   = MODE=00    (8채널 싱글엔드, Table 10)
 * bit7:6   = PM=00      (정상동작)
 * bit5     = Coding=0   (2의보수)
 * bit4     = Ref=1      (내부 기준전원 2.5V 사용)
 * bit3:2   = Seq=00     (직접 채널 선택, 시퀀서 없음)
 * bit1:0   = 0
 *
 * 0x8010 = bit15=1, bit9:8=00(MODE=8ch single-ended), bit4=1(내부ref)
 */
#define AD7327_CH_CTRL(ch)	((uint16_t)(0x8010U | ((uint16_t)(ch) << 10U)))


/* ── SPI 디바이스 ────────────────────────────────────────────── */
#define AD7327_NODE DT_NODELABEL(ad7327)

/* AD7327 SPI Mode 1 (CPOL=0, CPHA=1) */
static const struct spi_dt_spec s_spi = SPI_DT_SPEC_GET(
	AD7327_NODE,
	SPI_WORD_SET(16) | SPI_TRANSFER_MSB | SPI_OP_MODE_MASTER | SPI_MODE_CPHA,
	2
);

/* ── ADC 태스크 상수 ─────────────────────────────────────────── */
#define ADC_SAMPLE_MS		2
#define ADC_WINDOW_MS		2000
#define ADC_PRINT_EVERY_N	500
#define ADC_WINDOW_SAMPLES	(ADC_WINDOW_MS / ADC_SAMPLE_MS)
#define ADC_UDP_TX_INTERVAL_MS	20
#define ADC_UDP_SAMPLES_PER_PERIOD (ADC_UDP_TX_INTERVAL_MS / ADC_SAMPLE_MS)

BUILD_ASSERT((ADC_WINDOW_MS % ADC_SAMPLE_MS) == 0);
BUILD_ASSERT((ADC_UDP_TX_INTERVAL_MS % ADC_SAMPLE_MS) == 0);


/* ── 상태 변수 ───────────────────────────────────────────────── */
static adc_snapshot_t adc_snapshot;
static struct k_spinlock adc_lock;
static bool adc_has_sample;

static float running_min[ADC_MAX_CHANNELS];
static float running_max[ADC_MAX_CHANNELS];
static uint32_t window_sample_count;
static uint16_t udp_period_sample_count;
static uint16_t udp_frozen_sample_count;
static uint32_t adc_sem_give_fail;

K_THREAD_STACK_DEFINE(adc_task_stack, 8192);
static struct k_thread adc_task_data;

K_SEM_DEFINE(adc_sample_sem, 0, 128);
K_SEM_DEFINE(adc_udp_trigger_sem, 0, 1);

static void adc_sample_timer_handler(struct k_timer *timer);
K_TIMER_DEFINE(adc_sample_timer, adc_sample_timer_handler, NULL);

/* ── AD7327 SPI 헬퍼 ─────────────────────────────────────────── */

/* 16비트 단일 전송: CS 16비트 동안 LOW 유지
 * TX: ctrl_word 그대로 (LPSPI가 LE uint16_t를 bit15 먼저 전송)
 * RX: LPSPI가 수신 데이터를 LE로 저장 → sys_be16_to_cpu로 교정 */
static int ad7327_transfer(uint16_t ctrl_word, uint16_t *result)
{
	uint16_t tx = ctrl_word;
	uint16_t rx = 0;

	struct spi_buf tx_buf = { .buf = &tx, .len = sizeof(tx) };
	struct spi_buf rx_buf = { .buf = &rx, .len = sizeof(rx) };
	struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1 };
	struct spi_buf_set rx_set = { .buffers = &rx_buf, .count = 1 };

	int ret = spi_transceive_dt(&s_spi, &tx_set, &rx_set);

	/* tCONVERT = 16×tSCLK = 1.6μs (10MHz) → 16클록 동안 변환 완료
	 * spi_transceive_dt 반환 시 이미 완료, CS딜레이(2μs)로 충분 */

	if (ret == 0 && result) {
		/* RX: LPSPI가 수신 데이터를 LE로 저장
		 * sys_be16_to_cpu 없이 raw 값 사용 (바이트 순서 실험) */
		*result = rx;
	}
	return ret;
}

/* 결과어 파싱: [15:13]=채널번호, [12:1]=12bit 2의보수, [0]=0 */
static float ad7327_result_to_volts(uint16_t raw)
{
	uint16_t data12 = (raw >> 1) & 0x0FFFU;  /* bits[12:1] */
	int16_t code = (data12 & 0x0800U)
			? (int16_t)(data12 | 0xF000U)
			: (int16_t)data12;

	return (float)code * (AD7327_VREF_V / (float)AD7327_FULL_SCALE);
}


/* 8채널 읽기 - Read-ahead (2ms 1사이클):
 * 전송N과 수신N이 동시 발생 → 9회 전송으로 8채널 처리
 *   전송0: CH0명령 → 결과 폐기 (파이프라인 준비)
 *   전송1: CH1명령 → CH0 결과 수신
 *   전송2: CH2명령 → CH1 결과 수신
 *   ...
 *   전송8: CH0명령 → CH7 결과 수신
 * 총 9회 × 11.6μs = 104μs << 2ms */
static int ad7327_read_all(float *volts)
{
	uint16_t rx = 0;
	int ret;

	/* 전송0: 파이프라인 준비 (결과 폐기) */
	ret = ad7327_transfer(AD7327_CH_CTRL(0), &rx);
	if (ret != 0) {
		return ret;
	}

	/* 전송1~7: CH[i] 명령 + CH[i-1] 결과 수신 */
	for (int i = 1; i < AD7327_NUM_CHANNELS; i++) {
		ret = ad7327_transfer(AD7327_CH_CTRL(i), &rx);
		if (ret != 0) {
			return ret;
		}
		volts[i - 1] = ad7327_result_to_volts(rx);
	}

	/* 전송8: CH0 명령 + CH7 마지막 결과 수신 */
	ret = ad7327_transfer(AD7327_CH_CTRL(0), &rx);
	if (ret != 0) {
		return ret;
	}
	volts[7] = ad7327_result_to_volts(rx);

	return 0;
}

/* ── 샘플 처리 ───────────────────────────────────────────────── */

static void adc_sample_timer_handler(struct k_timer *timer)
{
	ARG_UNUSED(timer);

	if (k_sem_count_get(&adc_sample_sem) >= 127) {
		adc_sem_give_fail++;
	} else {
		k_sem_give(&adc_sample_sem);
	}
}

static void adc_publish_sample(const float *volts)
{
	k_spinlock_key_t key = k_spin_lock(&adc_lock);

	for (uint8_t i = 0; i < AD7327_NUM_CHANNELS; i++) {
		float v = volts[i];

		if (v < running_min[i]) {
			running_min[i] = v;
		}
		if (v > running_max[i]) {
			running_max[i] = v;
		}
		adc_snapshot.raw[i]     = v;
		adc_snapshot.min_val[i] = running_min[i];
		adc_snapshot.max_val[i] = running_max[i];
	}

	adc_snapshot.ch_count     = AD7327_NUM_CHANNELS;
	adc_snapshot.sample_count = udp_period_sample_count;
	get_datetime_ms(&adc_snapshot.datetime, &adc_snapshot.msec);

	k_spin_unlock(&adc_lock, key);
	adc_has_sample = true;
}

static void adc_begin_next_window(const float *volts)
{
	for (uint8_t i = 0; i < AD7327_NUM_CHANNELS; i++) {
		running_min[i] = volts[i];
		running_max[i] = volts[i];
	}
	window_sample_count = 0;
}

static void adc_process_one_sample(void)
{
	float volts[AD7327_NUM_CHANNELS];
	int ret = ad7327_read_all(volts);

	if (ret != 0) {
		printf("[ADC] AD7327 SPI read error: %d\n", ret);
		return;
	}

	if (udp_period_sample_count < 0xFFFFU) {
		udp_period_sample_count++;
	}
	window_sample_count++;
	adc_publish_sample(volts);

	if (window_sample_count >= ADC_WINDOW_SAMPLES) {
		adc_begin_next_window(volts);
	}

	if (udp_period_sample_count >= ADC_UDP_SAMPLES_PER_PERIOD) {
		k_spinlock_key_t tkey = k_spin_lock(&adc_lock);

		udp_frozen_sample_count = udp_period_sample_count;
		udp_period_sample_count = 0;
		k_spin_unlock(&adc_lock, tkey);
		k_sem_give(&adc_udp_trigger_sem);
	}
}

/* ── ADC 태스크 ──────────────────────────────────────────────── */

static void adc_task(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	if (!spi_is_ready_dt(&s_spi)) {
		printf("[ADC] AD7327 SPI not ready\n");
		return;
	}

	for (int i = 0; i < ADC_MAX_CHANNELS; i++) {
		running_min[i] = 999.0f;
		running_max[i] = -999.0f;
	}
	window_sample_count     = 0;
	udp_period_sample_count = 0;
	adc_sem_give_fail       = 0;

	strncpy(adc_snapshot.line, g_gw_config.master_code, ADC_LINE_ID_MAX_CHARS);
	adc_snapshot.line[ADC_LINE_ID_MAX_CHARS] = '\0';

	printf("[ADC] AD7327 8ch ±10V (SPI LPSPI6 P3_20-23)"
	       " %dms sample, %dms min/max, UDP %dms\n",
	       ADC_SAMPLE_MS, ADC_WINDOW_MS, ADC_UDP_TX_INTERVAL_MS);

	k_timer_start(&adc_sample_timer, K_MSEC(ADC_SAMPLE_MS), K_MSEC(ADC_SAMPLE_MS));

	while (1) {
		(void)k_sem_take(&adc_sample_sem, K_FOREVER);

		do {
			adc_process_one_sample();
		} while (k_sem_take(&adc_sample_sem, K_NO_WAIT) == 0);

		if (adc_sem_give_fail > 0U &&
		    (adc_sem_give_fail % ADC_PRINT_EVERY_N) == 0U) {
			printf("[ADC] sample sem overflow (lost ticks): %u\n",
			       (unsigned)adc_sem_give_fail);
		}
	}
}

/* ── 공개 API ────────────────────────────────────────────────── */

int adc_get_latest(adc_snapshot_t *out)
{
	if (!out || !adc_has_sample) {
		return -1;
	}
	k_spinlock_key_t key = k_spin_lock(&adc_lock);

	memcpy(out, &adc_snapshot, sizeof(adc_snapshot_t));
	out->sample_count = udp_frozen_sample_count;
	k_spin_unlock(&adc_lock, key);
	return 0;
}

int adc_task_start(void)
{
	k_tid_t tid = k_thread_create(&adc_task_data, adc_task_stack,
				      K_THREAD_STACK_SIZEOF(adc_task_stack),
				      adc_task, NULL, NULL, NULL, 3, 0, K_NO_WAIT);

	if (tid == NULL) {
		return -1;
	}
	k_thread_name_set(tid, "adc_task");
	return 0;
}
