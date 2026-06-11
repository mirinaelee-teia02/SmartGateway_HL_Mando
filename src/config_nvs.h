/*
 * SmartGateway — 런타임 설정 (NVS 영속 + 시리얼 메뉴)
 *
 * 목표: 장치코드(master_code)·인덱스(device_index, NVS 예약)·WiFi/ETH 주소·net_boot_mode
 * 를 플래시에 두고, 저장(S) 후 재부팅하면 선택 모드로 동작. (device_index는 UDP 페이로드와 무관)
 *
 * config_nvs_load() → Kconfig 기본값 후 NVS 덮어쓰기
 * config_nvs_menu() → 부팅 시 키 입력 시 편집
 * config_nvs_save() → NVS 기록
 */

#ifndef CONFIG_NVS_H
#define CONFIG_NVS_H

#include <stdint.h>

typedef struct {
	char     master_code[32];
	/* NVS 예약 필드(추후 프로토콜용). UDP MessagePack과는 무관 */
	uint16_t device_index;

	/* ── ETH 설정 ── */
	char     eth_ip[16];
	char     eth_netmask[16];
	char     eth_gw[16];
	char     eth_tcp_server_ip[16];
	uint16_t eth_tcp_server_port;
	char     eth_udp_server_ip[16];
	uint16_t eth_udp_server_port;

	/* ── WiFi 설정 ── */
	char     wifi_ssid[33];
	char     wifi_psk[65];
	char     wifi_ip[16];
	char     wifi_netmask[16];
	char     wifi_gw[16];
	char     wifi_tcp_server_ip[16];
	uint16_t wifi_tcp_server_port;
	char     wifi_udp_server_ip[16];
	uint16_t wifi_udp_server_port;

	/* NVS 영속: 0=WiFi 모드 1=이더넷 모드 (WiFi 실패 시 NVS를 1로 저장 후 재부팅) */
	uint8_t  net_boot_mode;

	/* 가스 센서 모델: 0=없음 1=O2-SM30 2=H2S-SM30 3=CO-SM30 4=S-300-3V 5=미정 */
	uint8_t  sensor_model_usb1; /* USB1(EHCI) 포트 */
	uint8_t  sensor_model_usb0; /* USB0(KHCI) 포트 */
} gw_config_t;

#define GW_NET_BOOT_WIFI 0U
#define GW_NET_BOOT_ETH  1U

extern gw_config_t g_gw_config;

void config_nvs_load(void);
void config_nvs_save(void);
void config_nvs_save_boot_mode(void);
void config_nvs_menu(void);

#endif /* CONFIG_NVS_H */
