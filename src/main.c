/*
 * 2026.03.18
 * Smart Gateway - Main
 */

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include "adc.h"
#include "rs232.h"
#include "tcp_gateway.h"
#include "udp.h"
#include <zephyr/autoconf.h>
#if IS_ENABLED(CONFIG_SMARTGATEWAY_WIFI_ENABLE)
#include "wifi_manager.h"
#endif

#define SmartGateway_VERSION	"V2.7.1"

#if IS_ENABLED(CONFIG_WIFI_NXP)
#  define NET_MODE_STR  "WiFi (IW416 / EVK-MAYA-W166)"
#else
#  define NET_MODE_STR  "Ethernet Only"
#endif

int main(void)
{
	printf("\n*** Smart Gateway %s ***\n", SmartGateway_VERSION);
	printf("Board: %s\n", CONFIG_BOARD);
	printf("Network: %s\n", NET_MODE_STR);
	printf("ADC: 8ch (raw/min/max), 2s window | UDP: %dms\n", UDP_SEND_INTERVAL_MS);
#if !IS_ENABLED(CONFIG_SMARTGATEWAY_NET_MODE_WIFI_ONLY)
	printf("ETH UDP: peer %s:%d\n", CONFIG_SMARTGATEWAY_ETH_UDP_PEER_IP,
	       CONFIG_SMARTGATEWAY_ETH_UDP_PEER_PORT);
	printf("ETH TCP: listen %s:%u | board IP %s\n",
	       CONFIG_SMARTGATEWAY_ETH_TCP_BIND_IP,
	       (unsigned)CONFIG_SMARTGATEWAY_ETH_TCP_LISTEN_PORT,
	       CONFIG_NET_CONFIG_MY_IPV4_ADDR);
#endif
#if !IS_ENABLED(CONFIG_SMARTGATEWAY_NET_MODE_ETH_ONLY)
	printf("WiFi UDP: peer %s:%d\n", CONFIG_SMARTGATEWAY_WIFI_UDP_PEER_IP,
	       CONFIG_SMARTGATEWAY_WIFI_UDP_PEER_PORT);
	printf("WiFi TCP: listen %s:%u | board IP %s\n",
	       CONFIG_SMARTGATEWAY_WIFI_TCP_BIND_IP,
	       (unsigned)CONFIG_SMARTGATEWAY_WIFI_TCP_LISTEN_PORT,
	       CONFIG_SMARTGATEWAY_WIFI_STATIC_IP);
#endif
	printf("  프레임: 5+Body+2(Err+ETX), 0x80↔0x00 핸드셰이크, 0x01→RS-232→0x81, 타임아웃 Err=E3\n");
#if IS_ENABLED(CONFIG_WIFI_NXP)
	printf("WiFi: IW416 / EVK-MAYA-W166 (SDIO only, auto-init)\n");
#endif
	printf("\n");

#if IS_ENABLED(CONFIG_SMARTGATEWAY_WIFI_ENABLE)
	if (wifi_task_start() != 0) {
		printf("[MAIN] Failed to create WiFi task\n");
		return -1;
	}
	printf("[MAIN] WiFi 연결 대기 중...\n");
	if (wifi_wait_ready(30000) != 0) {
		printf("[MAIN] WiFi 연결 실패 — ADC/UDP 시작 취소\n");
		return -1;
	}
	printf("[MAIN] WiFi 준비 완료 — ADC/UDP 시작\n");
#endif

	// ADC Task Start
	if (adc_task_start() != 0) {
		printf("[MAIN] Failed to create ADC task\n");
		return -1;
	}
	// RS-232 Task Start
//	if (rs232_task_start() != 0) {
//		printf("[MAIN] Failed to create RS-232 task\n");
//		return -1;
//	}
	// UDP Task Start
	if (udp_task_start() != 0) {
		printf("[MAIN] Failed to create UDP task\n");
		return -1;
	}
	// TCP Gateway Task Start
//	if (tcp_gateway_task_start() != 0) {
//		printf("[MAIN] Failed to create TCP gateway task\n");
//		return -1;
//	}

	while (1) {
		k_sleep(K_SECONDS(60));
	}

	CODE_UNREACHABLE;
}

