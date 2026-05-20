/*
 * Smart Gateway - ADC 모듈 구현
 *
 * - k_timer 2ms 주기로 샘플 트리거 (k_msleep 루프 지터 제거)
 * - 2초 윈도우 min/max (O(1)), 매 샘플 스냅샷 갱신 → UDP 20ms 전송용
 * - 채널 수: CONFIG_SMARTGATEWAY_ADC_CHANNEL_COUNT (1~8)
 *
 * [현재] 내부 LPADC0 — adc_inputs[] 는 보드 배선에 맞게 조정
 * [예정] AD7327 SPI 8ch — 하단 [AD7327 disabled] 블록
 */

#include "adc.h"
#include "config_nvs.h"
#include "time_helper.h"

#include <stdio.h>
#include <zephyr/autoconf.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/adc/mcux-lpadc.h>
#include <string.h>

#define ADC_NODE			DT_NODELABEL(lpadc0)
#define ADC_CHANNEL_COUNT		CONFIG_SMARTGATEWAY_ADC_CHANNEL_COUNT
#define ADC_SAMPLE_MS			2
#define ADC_WINDOW_MS			2000
#define ADC_PRINT_EVERY_N		500
#define VREF_MV				3300
#define ADC_RESOLUTION_BITS		12
#define ADC_FULL_SCALE_COUNTS		((1U << ADC_RESOLUTION_BITS) - 1U)
#define ADC_WINDOW_SAMPLES		(ADC_WINDOW_MS / ADC_SAMPLE_MS)
#define ADC_UDP_TX_INTERVAL_MS		20 /* must match UDP_SEND_INTERVAL_MS in udp.h */

BUILD_ASSERT(ADC_CHANNEL_COUNT >= 1);
BUILD_ASSERT(ADC_CHANNEL_COUNT <= ADC_MAX_CHANNELS);
BUILD_ASSERT(ADC_WINDOW_MS > ADC_SAMPLE_MS);
BUILD_ASSERT((ADC_WINDOW_MS % ADC_SAMPLE_MS) == 0);

/*
 * LPADC input_positive: MCUX_LPADC_CHxA/B (mcux-lpadc.h).
 * FRDM J4: Pin2=ADC0_A0(CH0A), Pin4=ADC0_B0(CH0B) — UDP ch1/ch2와 동일 순서.
 */
#if ADC_CHANNEL_COUNT == 2
static const uint8_t adc_inputs[ADC_MAX_CHANNELS] = {
	MCUX_LPADC_CH0A, MCUX_LPADC_CH0B, 0, 0, 0, 0, 0, 0,
};
#else
static const uint8_t adc_inputs[ADC_MAX_CHANNELS] = {
	0, 1, 2, 3, 4, 5, 6, 7,
};
#endif

static adc_snapshot_t adc_snapshot;
static struct k_spinlock adc_lock;
static bool adc_has_sample;

static uint16_t running_min[ADC_MAX_CHANNELS];
static uint16_t running_max[ADC_MAX_CHANNELS];
static uint32_t window_sample_count;   /* 2s min/max window */
static uint16_t udp_period_sample_count; /* samples since last UDP read (20ms) */
static uint32_t adc_sem_give_fail;

static int16_t adc_buffer[ADC_MAX_CHANNELS];
static struct adc_sequence adc_seq = {
	.buffer      = adc_buffer,
	.buffer_size = sizeof(adc_buffer),
	.resolution  = 12,
};

K_THREAD_STACK_DEFINE(adc_task_stack, 2048);
static struct k_thread adc_task_data;

/* RS-232 등으로 동일 우선순위 스레드가 막혀도 2ms×128ms 분량 버퍼 */
K_SEM_DEFINE(adc_sample_sem, 0, 128);

static void adc_sample_timer_handler(struct k_timer *timer);

K_TIMER_DEFINE(adc_sample_timer, adc_sample_timer_handler, NULL);

static inline float adc_counts_to_volts(uint16_t counts)
{
	return (float)counts * ((float)VREF_MV / 1000.0f) / (float)ADC_FULL_SCALE_COUNTS;
}

static void adc_sample_timer_handler(struct k_timer *timer)
{
	ARG_UNUSED(timer);

	if (k_sem_count_get(&adc_sample_sem) >= 127) {
		adc_sem_give_fail++;
	} else {
		k_sem_give(&adc_sample_sem);
	}
}

/* 매 샘플: raw + 현재 윈도우 min/max → UDP 20ms마다 최신값 전송 */
static void adc_publish_sample(const uint16_t *counts, uint8_t nch)
{
	k_spinlock_key_t key = k_spin_lock(&adc_lock);

	for (uint8_t i = 0; i < nch; i++) {
		uint16_t v = counts[i];

		if (v < running_min[i]) {
			running_min[i] = v;
		}
		if (v > running_max[i]) {
			running_max[i] = v;
		}
		adc_snapshot.raw[i] = adc_counts_to_volts(v);
		adc_snapshot.min_val[i] = adc_counts_to_volts(running_min[i]);
		adc_snapshot.max_val[i] = adc_counts_to_volts(running_max[i]);
	}
	for (uint8_t i = nch; i < ADC_MAX_CHANNELS; i++) {
		adc_snapshot.raw[i] = 0.0f;
		adc_snapshot.min_val[i] = 0.0f;
		adc_snapshot.max_val[i] = 0.0f;
	}

	adc_snapshot.ch_count = nch;
	adc_snapshot.sample_count = udp_period_sample_count;
	get_datetime(&adc_snapshot.datetime);
	adc_snapshot.msec = (uint16_t)(k_uptime_get_32() % 1000U);

	k_spin_unlock(&adc_lock, key);
	adc_has_sample = true;
}

/* 2초 윈도우 종료: 다음 윈도우 min/max 시드 */
static void adc_begin_next_window(const uint16_t *counts, uint8_t nch)
{
	for (uint8_t i = 0; i < nch; i++) {
		uint16_t v = counts[i];

		running_min[i] = v;
		running_max[i] = v;
	}
	window_sample_count = 0;
}

static void adc_process_one_sample(const struct device *adc_dev)
{
	int ret = adc_read(adc_dev, &adc_seq);

	if (ret != 0) {
		printf("[ADC] Read error: %d\n", ret);
		return;
	}

	const uint8_t nch = (uint8_t)ADC_CHANNEL_COUNT;
	uint16_t counts[ADC_MAX_CHANNELS];

	for (uint8_t i = 0; i < nch; i++) {
		counts[i] = (uint16_t)adc_buffer[i];
	}

	if (udp_period_sample_count < 0xFFFFU) {
		udp_period_sample_count++;
	}
	window_sample_count++;
	adc_publish_sample(counts, nch);

	if (window_sample_count >= ADC_WINDOW_SAMPLES) {
		adc_begin_next_window(counts, nch);
	}
}

static void adc_task(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	const struct device *adc_dev = DEVICE_DT_GET(ADC_NODE);

	if (!device_is_ready(adc_dev)) {
		printf("[ADC] Device not ready\n");
		return;
	}

	for (int i = 0; i < ADC_CHANNEL_COUNT; i++) {
		struct adc_channel_cfg ch_cfg = {
			.gain             = ADC_GAIN_1,
			.reference        = ADC_REF_EXTERNAL1,
			.acquisition_time = ADC_ACQ_TIME_DEFAULT,
			.channel_id       = i,
			.differential     = 0,
			.input_positive   = adc_inputs[i],
		};
		int ret = adc_channel_setup(adc_dev, &ch_cfg);

		if (ret < 0) {
			printf("[ADC] Ch%d setup failed: %d\n", i, ret);
			return;
		}
	}

	adc_seq.channels = BIT_MASK(ADC_CHANNEL_COUNT);
	adc_seq.buffer_size = (uint8_t)(ADC_CHANNEL_COUNT * sizeof(int16_t));

	for (int i = 0; i < ADC_MAX_CHANNELS; i++) {
		running_min[i] = 0xFFFF;
		running_max[i] = 0;
	}
	window_sample_count = 0;
	udp_period_sample_count = 0;
	adc_sem_give_fail = 0;

	strncpy(adc_snapshot.line, g_gw_config.master_code, ADC_LINE_ID_MAX_CHARS);
	adc_snapshot.line[ADC_LINE_ID_MAX_CHARS] = '\0';

	printf("[ADC] LPADC0: %dch (UDP ch", ADC_CHANNEL_COUNT);
	for (int i = 0; i < ADC_CHANNEL_COUNT; i++) {
		printf("%d=0x%02x", i + 1, adc_inputs[i]);
		if (i + 1 < ADC_CHANNEL_COUNT) {
			printf(" ");
		}
	}
	printf(") %dms sample, %dms min/max, UDP %dms\n", ADC_SAMPLE_MS, ADC_WINDOW_MS,
	       ADC_UDP_TX_INTERVAL_MS);

	k_timer_start(&adc_sample_timer, K_MSEC(ADC_SAMPLE_MS), K_MSEC(ADC_SAMPLE_MS));

	/*
	 * 타이머 2ms마다 sem 1개 — 쌓인 만큼 모두 adc_read (이전: batch만 세고 1회 읽어
	 * RS-232/tcp_gw 동시 동작 시 샘플 대량 유실).
	 */
	while (1) {
		(void)k_sem_take(&adc_sample_sem, K_FOREVER);

		do {
			adc_process_one_sample(adc_dev);
		} while (k_sem_take(&adc_sample_sem, K_NO_WAIT) == 0);

		if (adc_sem_give_fail > 0U &&
		    (adc_sem_give_fail % ADC_PRINT_EVERY_N) == 0U) {
			printf("[ADC] sample sem overflow (lost ticks): %u\n",
			       (unsigned)adc_sem_give_fail);
		}
	}
}

int adc_get_latest(adc_snapshot_t *out)
{
	if (!out || !adc_has_sample) {
		return -1;
	}
	k_spinlock_key_t key = k_spin_lock(&adc_lock);

	memcpy(out, &adc_snapshot, sizeof(adc_snapshot_t));
	udp_period_sample_count = 0;
	k_spin_unlock(&adc_lock, key);
	return 0;
}

int adc_task_start(void)
{
	/* prio 3: tcp_gw(6)·RS-232 Modbus 블로킹 중에도 2ms 샘플 선점 */
	k_tid_t tid = k_thread_create(&adc_task_data, adc_task_stack,
				      K_THREAD_STACK_SIZEOF(adc_task_stack),
				      adc_task, NULL, NULL, NULL, 3, 0, K_NO_WAIT);

	if (tid == NULL) {
		return -1;
	}
	k_thread_name_set(tid, "adc_task");
	return 0;
}

/* ════════════════════════════════════════════════════════════════
 * [AD7327 disabled] — SPI 8ch, overlay flexcomm6 참고 (기존 주석 블록)
 * ════════════════════════════════════════════════════════════════ */
