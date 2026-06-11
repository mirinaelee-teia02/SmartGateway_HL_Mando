/*
 * 2026.03.18
 * Smart Gateway - Main
 * 작성자 : LYH
 */

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/autoconf.h>

#include "adc.h"
#include "rs232.h"
#include "tcp_gateway.h"
#include "udp.h"
#include "config_nvs.h"
#include "network_manager.h"
#include "dip_switch.h"
#ifdef CONFIG_SMARTGATEWAY_DI_DO_ENABLE
#include "di_do.h"
#endif
#ifdef CONFIG_USB_HOST_STACK
#include "gas_sensor.h"
#endif

#define SmartGateway_VERSION "V2.0.0(HL-Mando)"

#if IS_ENABLED(CONFIG_WIFI_NXP)
#  define NET_MODE_STR "WiFi ([u-blox] MAYA-W160-001B(IW416))"
#else
#  define NET_MODE_STR "Ethernet Only"
#endif

int main(void)
{

	printf("\n*** Smart Gateway %s ***\n", SmartGateway_VERSION);
	printf("Network: %s\n", NET_MODE_STR);

	config_nvs_load();

	/* DIP 스위치: NVS 로드 후 필요 시 덮어쓰기 */
	dip_switch_apply();

	printf("[CFG] NVS summary - boot mode %u (%s)\n",
	       (unsigned int)g_gw_config.net_boot_mode,
	       g_gw_config.net_boot_mode == 1U ? "next boot Ethernet" : "next boot WiFi");
	printf("      Device code: %s  |  Index: %u\n", g_gw_config.master_code,
	       (unsigned)g_gw_config.device_index);
	printf("      WiFi  board %s | PC TCP %s:%u | UDP %s:%u\n",
	       g_gw_config.wifi_ip, g_gw_config.wifi_tcp_server_ip,
	       (unsigned)g_gw_config.wifi_tcp_server_port,
	       g_gw_config.wifi_udp_server_ip, (unsigned)g_gw_config.wifi_udp_server_port);
	printf("      ETH   board %s | PC TCP %s:%u | UDP %s:%u\n",
	       g_gw_config.eth_ip, g_gw_config.eth_tcp_server_ip,
	       (unsigned)g_gw_config.eth_tcp_server_port,
	       g_gw_config.eth_udp_server_ip, (unsigned)g_gw_config.eth_udp_server_port);

#ifdef CONFIG_SMARTGATEWAY_DI_DO_ENABLE
	if (di_do_init() != 0) {
		printf("[MAIN] DI/DO init failed\n");
	}
#endif

	/* USB HOST(FT2232 가스 센서): NVS 메뉴 전 시작 → 메뉴 내 T키로 즉시 확인 가능 */
#ifdef CONFIG_USB_HOST_STACK
	gas_sensor_start();
#endif

	config_nvs_menu();

	printf("[MAIN] NVS network mode: %u (0=WiFi 1=ETH)\n",
	       (unsigned int)g_gw_config.net_boot_mode);
	printf("ADC: %uch (raw/min/max), 2s window | UDP: %dms\n",
	       (unsigned)CONFIG_SMARTGATEWAY_ADC_CHANNEL_COUNT, UDP_SEND_INTERVAL_MS);

	/* ADC·RS-232: 네트워크 독립적 → 먼저 시작 */
	if (adc_task_start() != 0) {
		printf("[MAIN] Failed to create ADC task\n");
		return -1;
	}
	if (rs232_task_start() != 0) {
		printf("[MAIN] Failed to create RS-232 task\n");
		return -1;
	}

#ifdef CONFIG_SMARTGATEWAY_DI_DO_ENABLE
	if (di_do_task_start() != 0) {
		printf("[MAIN] DI/DO task start failed\n");
	}
#endif

	if (netmgr_start() != 0) {
		printf("[MAIN] netmgr_start failed\n");
		return -1;
	}

	printf("[MAIN] Waiting for network ready (max 180s)...\n");

	if (netmgr_wait_ready(180000) != 0) {
		/* 네트워크 타임아웃 — ADC·RS-232는 계속 동작, TCP·UDP만 중단 */
		printf("[MAIN] Network ready timeout — ADC/RS-232 continue without network\n");
		while (1) {
			k_sleep(K_SECONDS(60));
		}
	}

	printf("[MAIN] Network OK (%s) local IP %s\n",
	       netmgr_active_iface_label(), netmgr_local_ip());

	if (tcp_gateway_task_start() != 0) {
		printf("[MAIN] Failed to create TCP gateway task\n");
		return -1;
	}
	if (udp_task_start() != 0) {
		printf("[MAIN] Failed to create UDP task\n");
		return -1;
	}

	while (1) {
		k_sleep(K_SECONDS(60));
	}

	CODE_UNREACHABLE;
}
