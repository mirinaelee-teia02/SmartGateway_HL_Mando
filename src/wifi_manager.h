/*
 * SmartGateway — WiFi (NVS + netmgr)
 */
#ifndef WIFI_MANAGER_H_
#define WIFI_MANAGER_H_

#include <stdbool.h>

/** 레거시: 단일 태스크로 연결 → wifi_wait_ready 로 완료 대기 */
int wifi_task_start(void);
int wifi_wait_ready(int timeout_ms);

/** 분할 부팅(network_manager): 동기 한 번 연결·정적 IP */
int wifi_mgr_init(void);
int wifi_connect_once(int timeout_ms);
bool wifi_is_ready(void);
void wifi_force_disconnect(void);

#endif /* WIFI_MANAGER_H_ */
