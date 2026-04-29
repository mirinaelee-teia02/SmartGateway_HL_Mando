/*
 * SmartGateway — WiFi Task
 */
#ifndef WIFI_MANAGER_H_
#define WIFI_MANAGER_H_

/**
 * WiFi 태스크를 시작합니다.
 * AP 연결 → DHCP → 완료 순으로 진행됩니다.
 * @return 0 성공, 음수 실패
 */
int wifi_task_start(void);

/**
 * WiFi 준비 완료(AP 연결 + IP 설정)까지 블록.
 * @param timeout_ms 최대 대기 시간 (ms)
 * @return 0 성공, -EAGAIN 타임아웃
 */
int wifi_wait_ready(int timeout_ms);

#endif /* WIFI_MANAGER_H_ */
