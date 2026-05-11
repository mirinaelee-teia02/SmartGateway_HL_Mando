/*
 * WiFi Syslog — RFC 5424 UDP 전송
 *
 * WiFi 인터페이스(WIFI_STATIC_IP)에 bind → 라우팅을 WiFi로 고정.
 * ETH 모드에서도 Syslog는 항상 WiFi를 경유합니다.
 *
 * 사용:
 *   syslog_wifi_send(SLOG_INFO, "ADC", "ch0=1.23V ch1=2.34V");
 *   syslog_wifi_send(SLOG_ERR,  "TCP", "connect failed");
 */

#include "syslog_wifi.h"
#include "config_nvs.h"
#include "time_helper.h"
#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>
#include <zephyr/autoconf.h>
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>

/* RFC 5424: PRI = (Facility×8) | Severity, Facility=16(local0) */
#define SYSLOG_FACILITY  16
#define SYSLOG_BUF_SIZE  384   /* RFC 5424 최대 메시지 크기(480 권장 이하) */

#define SYSLOG_MODULE_MAX 16
#define SYSLOG_MSG_MAX    160

struct syslog_entry {
	int  severity;
	char module[SYSLOG_MODULE_MAX];
	char msg[SYSLOG_MSG_MAX];
};

/* 큐: 최대 8개 메시지 버퍼링, 넘치면 drop */
K_MSGQ_DEFINE(syslog_msgq, sizeof(struct syslog_entry), 8, 4);

#define SYSLOG_STACK_SIZE 1536
K_THREAD_STACK_DEFINE(syslog_stack, SYSLOG_STACK_SIZE);
static struct k_thread syslog_thr;

static bool syslog_ready;

void syslog_wifi_send(int severity, const char *module, const char *msg)
{
	struct syslog_entry e;

	e.severity = severity;
	strncpy(e.module, module ? module : "-", sizeof(e.module) - 1);
	e.module[sizeof(e.module) - 1] = '\0';
	strncpy(e.msg, msg ? msg : "", sizeof(e.msg) - 1);
	e.msg[sizeof(e.msg) - 1] = '\0';
	(void)k_msgq_put(&syslog_msgq, &e, K_NO_WAIT);
}

bool syslog_wifi_is_ready(void)
{
	return syslog_ready;
}

static void syslog_task(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	/* WiFi 연결 완료까지 대기 */
	while (!wifi_is_ready()) {
		k_msleep(500);
	}
	/* IP 안착 대기 */
	k_msleep(1000);

	int sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	if (sock < 0) {
		printf("[SYSLOG] socket() 실패 errno=%d\n", errno);
		return;
	}

	/* WiFi 인터페이스 고정: 보드 WiFi IP에 bind */
	struct sockaddr_in local = { 0 };

	local.sin_family = AF_INET;
	local.sin_port   = 0;
#if IS_ENABLED(CONFIG_SMARTGATEWAY_NET_MODE_WIFI_ONLY)
	if (zsock_inet_pton(AF_INET, g_gw_config.board_ip, &local.sin_addr) == 1) {
#else
	if (zsock_inet_pton(AF_INET, CONFIG_SMARTGATEWAY_WIFI_STATIC_IP, &local.sin_addr) == 1) {
#endif
		(void)zsock_bind(sock, (struct sockaddr *)&local, sizeof(local));
	}

	struct sockaddr_in srv = { 0 };

	srv.sin_family = AF_INET;
	srv.sin_port   = htons(g_gw_config.syslog_port);
	if (zsock_inet_pton(AF_INET, g_gw_config.syslog_ip,
			    &srv.sin_addr) != 1) {
		printf("[SYSLOG] 서버 IP 파싱 실패: %s\n",
		       g_gw_config.syslog_ip);
		zsock_close(sock);
		return;
	}

	printf("[SYSLOG] WiFi Syslog 준비: %s:%u\n",
	       g_gw_config.syslog_ip,
	       g_gw_config.syslog_port);

	syslog_ready = true;

	/* RFC 5424 포맷 버퍼 (BSS — 스택 절약) */
	static char buf[SYSLOG_BUF_SIZE];
	struct syslog_entry e;

	while (1) {
		/* 큐에서 메시지 꺼내기 (5초 타임아웃으로 WiFi 상태 주기 확인) */
		if (k_msgq_get(&syslog_msgq, &e, K_MSEC(5000)) != 0) {
			continue;
		}

		/* WiFi 끊긴 경우 재연결 대기 */
		if (!wifi_is_ready()) {
			syslog_ready = false;
			while (!wifi_is_ready()) {
				k_msleep(500);
			}
			k_msleep(500);
			syslog_ready = true;
		}

		datetime_t dt;

		get_datetime(&dt);

		int pri = (SYSLOG_FACILITY << 3) | (e.severity & 0x7);

		/* RFC 5424: <PRI>1 TIMESTAMP HOSTNAME APP-NAME PROCID MSGID SD MSG */
		int n = snprintf(buf, sizeof(buf),
				 "<%d>1 %04d-%02d-%02dT%02d:%02d:%02d.000Z"
				 " %s SmartGW - - - [%s] %s",
				 pri,
				 dt.year, dt.month, dt.day,
				 dt.hour, dt.min, dt.sec,
				 CONFIG_BOARD,
				 e.module, e.msg);

		if (n > 0 && n < (int)sizeof(buf)) {
			(void)zsock_sendto(sock, buf, (size_t)n, 0,
					   (struct sockaddr *)&srv, sizeof(srv));
		}
	}
}

int syslog_wifi_task_start(void)
{
	k_tid_t tid = k_thread_create(&syslog_thr, syslog_stack,
				       K_THREAD_STACK_SIZEOF(syslog_stack),
				       syslog_task, NULL, NULL, NULL,
				       12, 0, K_NO_WAIT);

	if (tid == NULL) {
		return -1;
	}
	k_thread_name_set(tid, "syslog_wifi");
	return 0;
}
