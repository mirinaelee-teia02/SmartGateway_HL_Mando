/*
 * SmartGateway — Network Manager
 *
 * WiFi 우선 → ETH 폴백 → 무한 순환 오케스트레이터.
 * TCP/UDP 태스크는 netmgr_* API로 현재 활성 인터페이스 정보를 조회한다.
 * ETH(유선)와 WiFi(무선) 보드 IP는 g_gw_config 의 서로 다른 필드(eth_* / wifi_*).
 */

#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
	NETMGR_IFACE_NONE = 0,
	NETMGR_IFACE_WIFI,
	NETMGR_IFACE_ETH,
} netmgr_iface_t;

/* 네트워크 매니저 태스크 시작 (main에서 한 번만 호출) */
int netmgr_start(void);

/* 현재 인터페이스가 준비(IP 설정 + 통신 가능) 상태인지 확인 */
bool netmgr_is_ready(void);

/* 준비 완료까지 블록. 0=성공, -1=타임아웃 */
int netmgr_wait_ready(int timeout_ms);

/* 현재 활성 인터페이스 종류 */
netmgr_iface_t netmgr_active_iface(void);

/* 활성 인터페이스 로그용 라벨: "WiFi", "ETH", "(none)" */
const char *netmgr_active_iface_label(void);

/* 현재 활성 인터페이스의 보드 IP (TCP 소켓 bind 용) */
const char *netmgr_local_ip(void);

/* TCP 피어 IP / 포트 */
const char *netmgr_tcp_peer_ip(void);
uint16_t    netmgr_tcp_peer_port(void);

/* UDP 피어 IP / 포트 */
const char *netmgr_udp_peer_ip(void);
uint16_t    netmgr_udp_peer_port(void);

#endif /* NETWORK_MANAGER_H */
