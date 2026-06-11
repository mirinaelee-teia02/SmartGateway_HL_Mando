/*
 * SmartGateway — DI/DO I2C GPIO 확장기 (PCA9554DB)
 *
 * DO: LPI2C1(FC1) P0_20(SDA)/P0_21(SCL) → PCA9554DB addr=0x22, 8채널 출력
 * DI: LPI2C7(FC7) P3_2(SDA)/P3_3(SCL)   → PCA9554DB addr=0x21, 8채널 입력
 * EXT_PWR1: GPIO0 pin24, EXT_PWR2: GPIO0 pin25
 * DO_INT:   GPIO3 pin0 (Active-Low, 폴링 모드 — 향후 인터럽트 확장)
 * DI_INT:   GPIO3 pin1 (Active-Low, 폴링 모드)
 */

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "di_do.h"

LOG_MODULE_REGISTER(di_do, LOG_LEVEL_INF);

/* PCA9554DB I2C 주소 */
#define PCA9554_DI_ADDR   0x21U  /* A0=3.3V(1), A1=0V(0), A2=0V(0) */
#define PCA9554_DO_ADDR   0x22U  /* A0=0V(0),   A1=3.3V(1), A2=0V(0) */

/* PCA9554 레지스터 */
#define PCA9554_REG_INPUT    0x00U  /* 입력 포트 (read) */
#define PCA9554_REG_OUTPUT   0x01U  /* 출력 래치 (read/write) */
#define PCA9554_REG_POLARITY 0x02U  /* 극성 반전 (0=미반전) */
#define PCA9554_REG_CONFIG   0x03U  /* 방향 (0=출력, 1=입력, default=0xFF) */

/* GPIO 핀 번호 */
#define EXT_PWR1_PIN 24U  /* P0_24 */
#define EXT_PWR2_PIN 25U  /* P0_25 */
#define DO_INT_PIN    0U  /* P3_0 */
#define DI_INT_PIN    1U  /* P3_1 */

#define DI_DO_STACK_SIZE 1024U
#define DI_DO_PRIORITY      7
#define DI_DO_POLL_MS      50U   /* DI 폴링 주기 */

static const struct device *const s_do_i2c = DEVICE_DT_GET(DT_NODELABEL(flexcomm1_lpi2c1));
static const struct device *const s_di_i2c = DEVICE_DT_GET(DT_NODELABEL(flexcomm7_lpi2c7));
static const struct device *const s_gpio0  = DEVICE_DT_GET(DT_NODELABEL(gpio0));
static const struct device *const s_gpio3  = DEVICE_DT_GET(DT_NODELABEL(gpio3));

static uint8_t s_do_val;
static uint8_t s_di_val;
static bool    s_pwr1_on;
static bool    s_pwr2_on;

/* ── PCA9554 I2C 레지스터 접근 ──────────────────────────────────────── */

static int pca_wr(const struct device *dev, uint8_t addr, uint8_t reg, uint8_t val)
{
	return i2c_reg_write_byte(dev, addr, reg, val);
}

static int pca_rd(const struct device *dev, uint8_t addr, uint8_t reg, uint8_t *val)
{
	return i2c_reg_read_byte(dev, addr, reg, val);
}

/* ── 공개 API ────────────────────────────────────────────────────────── */

int do_set(uint8_t mask)
{
	/* DO 출력은 active-LOW: mask bit=1=ON → pin LOW(~mask) */
	int ret = pca_wr(s_do_i2c, PCA9554_DO_ADDR, PCA9554_REG_OUTPUT, (uint8_t)(~mask));

	if (ret == 0) {
		s_do_val = mask;
		LOG_DBG("DO=0x%02X", mask);
	} else {
		LOG_ERR("DO set fail: %d", ret);
	}
	return ret;
}

uint8_t do_get(void)
{
	return s_do_val;
}

int di_read(uint8_t *val)
{
	int ret = pca_rd(s_di_i2c, PCA9554_DI_ADDR, PCA9554_REG_INPUT, val);

	if (ret == 0) {
		s_di_val = *val;
	}
	return ret;
}

uint8_t di_get(void)
{
	return s_di_val;
}

void ext_pwr1_set(bool on)
{
	gpio_pin_set(s_gpio0, EXT_PWR1_PIN, on ? 1 : 0);
	s_pwr1_on = on;
	LOG_INF("EXT_PWR1 %s", on ? "ON" : "OFF");
}

void ext_pwr2_set(bool on)
{
	gpio_pin_set(s_gpio0, EXT_PWR2_PIN, on ? 1 : 0);
	s_pwr2_on = on;
	LOG_INF("EXT_PWR2 %s", on ? "ON" : "OFF");
}

bool ext_pwr1_get(void) { return s_pwr1_on; }
bool ext_pwr2_get(void) { return s_pwr2_on; }

/* ── 초기화 ─────────────────────────────────────────────────────────── */

static int di_do_hw_init(void)
{
	int ret;

	/* DI: 전체 8채널 입력, active-LOW → 극성 반전 (0xFF) */
	ret = pca_wr(s_di_i2c, PCA9554_DI_ADDR, PCA9554_REG_POLARITY, 0xFFU);
	if (ret != 0) {
		LOG_ERR("DI polarity: %d", ret);
		return ret;
	}
	ret = pca_wr(s_di_i2c, PCA9554_DI_ADDR, PCA9554_REG_CONFIG, 0xFFU);
	if (ret != 0) {
		LOG_ERR("DI config: %d", ret);
		return ret;
	}

	/* DO: 전체 8채널 출력, 초기 전체 OFF (active-LOW → pin HIGH = 0xFF) */
	s_do_val = 0x00U;
	ret = pca_wr(s_do_i2c, PCA9554_DO_ADDR, PCA9554_REG_OUTPUT, (uint8_t)(~s_do_val));
	if (ret != 0) {
		LOG_ERR("DO output: %d", ret);
		return ret;
	}
	ret = pca_wr(s_do_i2c, PCA9554_DO_ADDR, PCA9554_REG_CONFIG, 0x00U);
	if (ret != 0) {
		LOG_ERR("DO config: %d", ret);
		return ret;
	}

	LOG_INF("PCA9554 DI(0x%02X) DO(0x%02X) init OK",
		PCA9554_DI_ADDR, PCA9554_DO_ADDR);
	return 0;
}

int di_do_init(void)
{
	if (!device_is_ready(s_do_i2c)) {
		LOG_ERR("DO I2C (lpi2c1) not ready");
		return -ENODEV;
	}
	if (!device_is_ready(s_di_i2c)) {
		LOG_ERR("DI I2C (lpi2c7) not ready");
		return -ENODEV;
	}
	if (!device_is_ready(s_gpio0)) {
		LOG_ERR("GPIO0 not ready");
		return -ENODEV;
	}
	if (!device_is_ready(s_gpio3)) {
		LOG_ERR("GPIO3 not ready");
		return -ENODEV;
	}

	/* EXT_PWR1/2: 출력, 초기 OFF */
	gpio_pin_configure(s_gpio0, EXT_PWR1_PIN, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure(s_gpio0, EXT_PWR2_PIN, GPIO_OUTPUT_INACTIVE);

	/* INT 핀: 입력 (현재 폴링 미사용, 향후 확장) */
	gpio_pin_configure(s_gpio3, DO_INT_PIN, GPIO_INPUT);
	gpio_pin_configure(s_gpio3, DI_INT_PIN, GPIO_INPUT);

	if (di_do_hw_init() != 0) {
		return -EIO;
	}

	ext_pwr1_set(false);
	ext_pwr2_set(false);

	return 0;
}

/* ── 태스크 ─────────────────────────────────────────────────────────── */

static void di_do_task(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	uint8_t di_val  = 0U;
	uint8_t di_prev = 0xFFU;

	LOG_INF("di_do_task started (poll %ums)", DI_DO_POLL_MS);

	while (1) {
		/* DI 폴링: 상태 변화 시 콘솔 자동 출력 */
		if (di_read(&di_val) == 0) {
			if (di_val != di_prev) {
				printf("\r\n[DI] 0x%02X -> 0x%02X  [", di_prev, di_val);
				for (int i = 0; i <= 7; i++) {
					printf("%d", (di_val >> i) & 1);
				}
				printf("]\r\n");
				di_prev = di_val;
			}
		}

		k_sleep(K_MSEC(DI_DO_POLL_MS));
	}
}

static K_THREAD_STACK_DEFINE(s_di_do_stack, DI_DO_STACK_SIZE);
static struct k_thread s_di_do_thread;

int di_do_task_start(void)
{
	k_tid_t tid = k_thread_create(&s_di_do_thread, s_di_do_stack,
				       DI_DO_STACK_SIZE,
				       di_do_task, NULL, NULL, NULL,
				       DI_DO_PRIORITY, 0, K_NO_WAIT);
	if (tid == NULL) {
		LOG_ERR("thread create failed");
		return -1;
	}
	k_thread_name_set(tid, "di_do_task");
	return 0;
}
