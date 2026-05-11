/*
 * Smart Gateway - ADC 모듈 구현
 * 8채널 지원, 2초 윈도우 min/max 추적
 *
 * [현재] 내부 LPADC0 2채널 검증 모드
 * [예정] AD7327 외부 SPI 8채널 — 하단 [AD7327 disabled] 블록 참조
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
#include <string.h>

#define ADC_NODE			DT_NODELABEL(lpadc0)
#define ADC_CHANNEL_COUNT		2   /* 2(Test) */
#define ADC_READ_INTERVAL_MS		2
#define ADC_WINDOW_MS			2000   /* min/max 윈도우 (ms) */
#define ADC_PRINT_EVERY_N		100
#define VREF_MV				3300   /* 3.3V */
#define ADC_RESOLUTION_BITS		12
#define ADC_FULL_SCALE_COUNTS		((1U << ADC_RESOLUTION_BITS) - 1U)

/* LPADC 카운트(0~fullscale) → 볼트 (0 ~ Vref) */
static inline float adc_counts_to_volts(uint16_t counts)
{
	return (float)counts * ((float)VREF_MV / 1000.0f) / (float)ADC_FULL_SCALE_COUNTS;
}

/* LPADC input_positive: CH0A,CH0B (확장시 보드 스키매틱 참고) */
static const uint8_t adc_inputs[] = { 0, 0x20 };

static adc_snapshot_t adc_snapshot;
static struct k_spinlock adc_lock;
static bool adc_has_sample;

/* 2초 윈도우 min/max (실시간 추적, 링버퍼 불필요 - O(1) 업데이트) */
static uint16_t running_min[ADC_MAX_CHANNELS];
static uint16_t running_max[ADC_MAX_CHANNELS];
static uint32_t window_sample_count;

static int16_t adc_buffer[ADC_CHANNEL_COUNT];
static struct adc_sequence adc_seq = {
	.buffer      = adc_buffer,
	.buffer_size = sizeof(adc_buffer),
	.resolution  = 12,
};

K_THREAD_STACK_DEFINE(adc_task_stack, 1536);
static struct k_thread adc_task_data;

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

	/* running min/max 초기화 */
	for (int i = 0; i < ADC_MAX_CHANNELS; i++) {
		running_min[i] = 0xFFFF;
		running_max[i] = 0;
	}
	window_sample_count = 0;

	strncpy(adc_snapshot.line, g_gw_config.master_code, ADC_LINE_ID_MAX_CHARS);
	adc_snapshot.line[ADC_LINE_ID_MAX_CHARS] = '\0';

	printf("[ADC] LPADC0 시작: %dch, %dms 사이클, %dms 윈도우\n",
	       ADC_CHANNEL_COUNT, ADC_READ_INTERVAL_MS, ADC_WINDOW_MS);

	while (1) {
		int ret = adc_read(adc_dev, &adc_seq);

		if (ret == 0) {
			/* 실시간 min/max 갱신 */
			for (int i = 0; i < ADC_CHANNEL_COUNT; i++) {
				uint16_t v = (uint16_t)adc_buffer[i];

				if (v < running_min[i])
					running_min[i] = v;
				if (v > running_max[i])
					running_max[i] = v;
			}
			window_sample_count++;

			/* 2초 윈도우 경과 시 스냅샷 저장 */
			uint32_t elapsed = window_sample_count * ADC_READ_INTERVAL_MS;

			if (elapsed >= ADC_WINDOW_MS) {
				k_spinlock_key_t key = k_spin_lock(&adc_lock);

				get_datetime(&adc_snapshot.datetime);
				adc_snapshot.msec = (uint16_t)(k_uptime_get_32() % 1000U);
				strncpy(adc_snapshot.line, g_gw_config.master_code, ADC_LINE_ID_MAX_CHARS);
				adc_snapshot.line[ADC_LINE_ID_MAX_CHARS] = '\0';
				for (int i = 0; i < ADC_CHANNEL_COUNT; i++) {
					uint16_t v = (uint16_t)adc_buffer[i];

					adc_snapshot.raw[i]     = adc_counts_to_volts(v);
					adc_snapshot.min_val[i] = adc_counts_to_volts(running_min[i]);
					adc_snapshot.max_val[i] = adc_counts_to_volts(running_max[i]);
					running_min[i] = v;
					running_max[i] = v;
				}
				adc_snapshot.ch_count = ADC_CHANNEL_COUNT;
				adc_has_sample = true;
				k_spin_unlock(&adc_lock, key);
				window_sample_count = 0;
			}
		} else {
			printf("[ADC] Read error: %d\n", ret);
		}
		k_msleep(ADC_READ_INTERVAL_MS);
	}
}

int adc_get_latest(adc_snapshot_t *out)
{
	if (!out || !adc_has_sample)
		return -1;
	k_spinlock_key_t key = k_spin_lock(&adc_lock);

	memcpy(out, &adc_snapshot, sizeof(adc_snapshot_t));
	k_spin_unlock(&adc_lock, key);
	return 0;
}

int adc_task_start(void)
{
	k_tid_t tid = k_thread_create(&adc_task_data, adc_task_stack,
				      K_THREAD_STACK_SIZEOF(adc_task_stack),
				      adc_task, NULL, NULL, NULL, 5, 0, K_NO_WAIT);
	if (tid == NULL)
		return -1;
	k_thread_name_set(tid, "adc_task");
	return 0;
}

/* ════════════════════════════════════════════════════════════════
 * [AD7327 disabled] 외부 SPI 8채널 ADC — CONFIG_SPI=y 시 활성화
 * ════════════════════════════════════════════════════════════════
 *
 * #include <zephyr/drivers/spi.h>
 *
 * #define AD7327_NODE         DT_NODELABEL(ad7327)
 * #define AD7327_CTRL_WORD    0x9C18U
 * #define AD7327_CH_SHIFT     13
 * #define AD7327_DATA_MASK    0x1FFFU
 * #define AD7327_SIGN_BIT     0x1000U
 * #define ADC_CHANNEL_COUNT   ADC_MAX_CHANNELS   // 8ch
 * #define VREF_MV             10000              // ±10V
 *
 * static const struct spi_dt_spec ad7327_spi =
 *     SPI_DT_SPEC_GET(AD7327_NODE,
 *                     SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB | SPI_WORD_SET(8), 0);
 *
 * static inline int16_t ad7327_sign_extend(uint16_t raw13) {
 *     return (raw13 & AD7327_SIGN_BIT) ? (int16_t)(raw13 | 0xE000U) : (int16_t)raw13;
 * }
 * static inline float ad7327_counts_to_volts(int16_t counts) {
 *     return (float)counts * (10.0f / 4096.0f);
 * }
 * static int ad7327_transfer(uint16_t tx, uint16_t *rx_out) { ... }
 *
 * adc_task() 초기화:
 *     spi_is_ready_dt(&ad7327_spi)
 *     ad7327_transfer(AD7327_CTRL_WORD, &dummy)  // 제어 레지스터 0x9C18
 *     for 8회 ad7327_transfer(0, &dummy)          // 파이프라인 플러시
 *
 * adc_task() 루프 (ADC_MAX_CHANNELS = 8):
 *     ad7327_transfer(0x0000, &rx)
 *     int ch = (rx >> AD7327_CH_SHIFT) & 0x7
 *     float v = ad7327_counts_to_volts(ad7327_sign_extend(rx & AD7327_DATA_MASK))
 *
 * overlay (boards/frdm_mcxn947_mcxn947_cpu0.overlay):
 *     &flexcomm6_lpspi6 { status = "okay";
 *         ad7327: ad7327@0 { compatible = "vnd,spi-device"; reg = <0>;
 *                            spi-max-frequency = <10000000>; }; };
 * ════════════════════════════════════════════════════════════════ */
