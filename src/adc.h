/*
 * Smart Gateway - ADC 모듈 헤더
 *
 * [역할]
 * AD7327 외부 SPI ADC (8채널 ±10V)를 사용하는 인터페이스 선언.
 * adc_task_start() 로 샘플링 스레드를 시작하고,
 * adc_get_latest() 로 최신 스냅샷을 읽습니다.
 *
 * [핀 연결] LPSPI6 (FlexComm6, MikroBUS SPI)
 *   P3_20=MOSI  P3_21=SCLK  P3_22=MISO  P3_23=CS(PCS0)
 */

#ifndef ADC_H
#define ADC_H

#include <stdint.h>

#define ADC_MAX_CHANNELS  8

/* line / DeviceID: 본문 최대 50자 + NUL (Kconfig SMARTGATEWAY_LINE_ID 등) */
#define ADC_LINE_ID_MAX_CHARS  50
#define ADC_LINE_MAX_LEN       (ADC_LINE_ID_MAX_CHARS + 1)

/* 날짜/시간 (년/월/일/시/분/초) */
typedef struct {
	uint16_t year;   /* 2000~2099 */
	uint8_t  month;  /* 1~12 */
	uint8_t  day;    /* 1~31 */
	uint8_t  hour;   /* 0~23 */
	uint8_t  min;    /* 0~59 */
	uint8_t  sec;    /* 0~59 */
} datetime_t;

/* ADC 스냅샷 — 2ms 샘플, 2s min/max 윈도우, UDP는 20ms마다 최신 raw+min/max 전송 */
typedef struct {
	char       line[ADC_LINE_MAX_LEN];       /* 라인 ID 문자열, NUL 종료 (NVS master_code와 동기) */
	datetime_t datetime;                     /* 전송 시각 (초) */
	uint16_t   msec;                         /* UDP 타임스탬프 17자(밀리초 3자)용 */
	uint16_t   sample_count;                 /* since last UDP TX (~20ms / 2ms ≈ 10) */
	float      raw[ADC_MAX_CHANNELS];        /* 최신 샘플 전압 (V, 0~Vref) */
	float      min_val[ADC_MAX_CHANNELS];    /* 2초 윈도우 최소 전압 (V) */
	float      max_val[ADC_MAX_CHANNELS];    /* 2초 윈도우 최대 전압 (V) */
	uint8_t    ch_count;                     /* 2~8 */
} adc_snapshot_t;

/*
 * adc_task_start
 *
 * ADC 샘플링을 수행하는 전용 스레드를 생성하고 시작합니다.
 * k_timer(2ms)로 ADC를 읽고, 매 샘플 공유 스냅샷(raw·min·max)을 갱신합니다.
 *
 */
int adc_task_start(void);

/*
 * adc_get_latest
 *
 * 현재 ADC 스냅샷의 복사본을 반환합니다.
 * UDP Task 등에서 호출하여 최신값을 읽습니다.
 */
int adc_get_latest(adc_snapshot_t *out);

#endif /* ADC_H */
