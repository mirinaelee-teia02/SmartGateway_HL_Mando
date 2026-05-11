#ifndef SYSLOG_WIFI_H_
#define SYSLOG_WIFI_H_

#include <stdbool.h>

/* RFC 5424 Severity (Facility=16 local0) */
#define SLOG_ERR     3
#define SLOG_WARNING 4
#define SLOG_NOTICE  5
#define SLOG_INFO    6
#define SLOG_DEBUG   7

/**
 * WiFi Syslog 태스크를 시작합니다.
 * WiFi 연결 후 자동으로 UDP 소켓을 열고 서버에 연결합니다.
 * CONFIG_SMARTGATEWAY_SYSLOG_ENABLE=y 시에만 호출합니다.
 */
int syslog_wifi_task_start(void);

/**
 * Syslog 메시지를 전송합니다 (스레드 안전, 비블록).
 * 큐가 꽉 찼으면 메시지를 버립니다.
 * @param severity  SLOG_ERR / SLOG_WARNING / SLOG_INFO / SLOG_DEBUG
 * @param module    모듈 이름 (최대 15자, NULL이면 "-")
 * @param msg       메시지 문자열 (최대 159자)
 */
void syslog_wifi_send(int severity, const char *module, const char *msg);

/** Syslog 소켓이 열려 있으면 true */
bool syslog_wifi_is_ready(void);

#endif /* SYSLOG_WIFI_H_ */
