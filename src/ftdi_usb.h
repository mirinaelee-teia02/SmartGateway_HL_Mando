/* FTDI FT2232 USB Host Class Driver — SmartGateway
 *
 * FT2232D (VID=0x0403 PID=0x6001) or FT2232H (PID=0x6010)
 * Channel A → 가스 센서 UART 38400 8N1
 *
 * 의존: CONFIG_UHC_NXP_EHCI=y (USB1), CONFIG_UHC_NXP_KHCI=y (USB0)
 *       CONFIG_USB_HOST_STACK=y
 */

#ifndef FTDI_USB_H
#define FTDI_USB_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	FTDI_PORT_USB1 = 0, /* USB1 EHCI HS — CON201 포트1 */
	FTDI_PORT_USB0 = 1, /* USB0 KHCI FS — CON201 포트2 */
	FTDI_PORT_COUNT
} ftdi_port_t;

/* 수신 콜백 — UART 데이터 바이트 도착 시 호출 */
typedef void (*ftdi_rx_cb_t)(ftdi_port_t port, const uint8_t *data,
			     size_t len, void *user);

/**
 * @brief 지정 USB 포트에 대해 USB HOST 초기화 및 FTDI 폴링 스레드 시작
 *
 * @param port   FTDI_PORT_USB1 또는 FTDI_PORT_USB0
 * @param cb     UART 수신 콜백 (NULL 불가)
 * @param user   콜백에 전달할 사용자 포인터
 * @return 0 성공, 음수 에러
 */
int ftdi_usb_init(ftdi_port_t port, ftdi_rx_cb_t cb, void *user);

/**
 * @brief 지정 포트의 FT2232 연결 여부
 */
bool ftdi_usb_connected(ftdi_port_t port);

#ifdef __cplusplus
}
#endif

#endif /* FTDI_USB_H */
