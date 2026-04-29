/*
 * SmartGateway — WiFi Task
 * AP 연결 → 정적 IP 설정 → 완료
 */

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "wifi_manager.h"

LOG_MODULE_REGISTER(wifi_mgr, LOG_LEVEL_INF);

#define WIFI_STACK_SIZE     4096
#define WIFI_PRIORITY       5

#define CONNECT_TIMEOUT_S   30

/* ── 이벤트 세마포어 ─────────────────────────────────── */
static K_SEM_DEFINE(sem_connected, 0, 1);
static K_SEM_DEFINE(sem_wifi_ready, 0, 1);  /* IP 설정 완료 후 give */

static struct net_mgmt_event_callback wifi_cb;

/* ── WiFi 이벤트 콜백 ────────────────────────────────── */
static void on_wifi_event(struct net_mgmt_event_callback *cb,
			  uint64_t event, struct net_if *iface)
{
	if (event == NET_EVENT_WIFI_CONNECT_RESULT) {
		const struct wifi_status *st =
			(const struct wifi_status *)cb->info;
		if (st->status == 0) {
			LOG_INF("WiFi connected: SSID=%s",
				CONFIG_SMARTGATEWAY_WIFI_SSID);
			k_sem_give(&sem_connected);
		} else {
			LOG_ERR("WiFi connect failed (status=%d)", st->status);
		}
	} else if (event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
		LOG_WRN("WiFi disconnected");
	}
}

/* ── 정적 IP 설정 ────────────────────────────────────── */
static int wifi_set_static_ip(struct net_if *iface)
{
	struct in_addr addr, mask, gw;

	if (net_addr_pton(AF_INET, CONFIG_SMARTGATEWAY_WIFI_STATIC_IP, &addr) ||
	    net_addr_pton(AF_INET, CONFIG_SMARTGATEWAY_WIFI_STATIC_NETMASK, &mask) ||
	    net_addr_pton(AF_INET, CONFIG_SMARTGATEWAY_WIFI_STATIC_GW, &gw)) {
		LOG_ERR("IP 주소 파싱 실패");
		return -EINVAL;
	}

	if (!net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0)) {
		LOG_ERR("IP 주소 설정 실패");
		return -ENODEV;
	}

	net_if_ipv4_set_netmask_by_addr(iface, &addr, &mask);
	net_if_ipv4_set_gw(iface, &gw);

	/* WiFi를 기본 인터페이스로 설정 → UDP 라우팅이 WiFi를 우선 선택 */
	net_if_set_default(iface);

	LOG_INF("WiFi 정적 IP: %s / %s  GW: %s (기본 인터페이스 설정)",
		CONFIG_SMARTGATEWAY_WIFI_STATIC_IP,
		CONFIG_SMARTGATEWAY_WIFI_STATIC_NETMASK,
		CONFIG_SMARTGATEWAY_WIFI_STATIC_GW);
	return 0;
}

/* ── WiFi 태스크 ─────────────────────────────────────── */
static void wifi_task(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	LOG_INF("WiFi 태스크 시작");

	/* WiFi 인터페이스 획득 */
	struct net_if *iface = net_if_get_first_wifi();

	if (!iface) {
		LOG_ERR("WiFi 인터페이스 없음");
		return;
	}

	/* 이벤트 콜백 등록 */
	net_mgmt_init_event_callback(&wifi_cb, on_wifi_event,
		NET_EVENT_WIFI_CONNECT_RESULT |
		NET_EVENT_WIFI_DISCONNECT_RESULT);
	net_mgmt_add_event_callback(&wifi_cb);

	/* wpa_supplicant 준비 대기 */
	k_sleep(K_SECONDS(2));

	/* ── AP 연결 파라미터 ─── */
	static struct wifi_connect_req_params params;

	params.ssid        = CONFIG_SMARTGATEWAY_WIFI_SSID;
	params.ssid_length = strlen(CONFIG_SMARTGATEWAY_WIFI_SSID);
	params.channel     = WIFI_CHANNEL_ANY;
	params.band        = WIFI_FREQ_BAND_UNKNOWN;
	params.mfp         = WIFI_MFP_OPTIONAL;

#if IS_ENABLED(CONFIG_SMARTGATEWAY_WIFI_SECURITY_WPA3)
	params.sae_password        = CONFIG_SMARTGATEWAY_WIFI_PSK;
	params.sae_password_length = strlen(CONFIG_SMARTGATEWAY_WIFI_PSK);
	params.security            = WIFI_SECURITY_TYPE_SAE_AUTO;
#elif IS_ENABLED(CONFIG_SMARTGATEWAY_WIFI_SECURITY_WPA_AUTO)
	params.psk                 = CONFIG_SMARTGATEWAY_WIFI_PSK;
	params.psk_length          = strlen(CONFIG_SMARTGATEWAY_WIFI_PSK);
	params.sae_password        = CONFIG_SMARTGATEWAY_WIFI_PSK;
	params.sae_password_length = strlen(CONFIG_SMARTGATEWAY_WIFI_PSK);
	params.security            = WIFI_SECURITY_TYPE_WPA_AUTO_PERSONAL;
#elif IS_ENABLED(CONFIG_SMARTGATEWAY_WIFI_SECURITY_WPA2)
	params.psk        = CONFIG_SMARTGATEWAY_WIFI_PSK;
	params.psk_length = strlen(CONFIG_SMARTGATEWAY_WIFI_PSK);
	params.security   = WIFI_SECURITY_TYPE_PSK;
#else /* NONE */
	params.security = WIFI_SECURITY_TYPE_NONE;
#endif

	LOG_INF("AP 연결 중: '%s' (보안=%d) ...",
		CONFIG_SMARTGATEWAY_WIFI_SSID, params.security);

	int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface,
			   &params, sizeof(params));
	if (ret) {
		LOG_ERR("connect 요청 실패: %d", ret);
		return;
	}

	/* ── AP 연결 대기 ─── */
	if (k_sem_take(&sem_connected, K_SECONDS(CONNECT_TIMEOUT_S)) != 0) {
		LOG_ERR("AP 연결 타임아웃 (%ds)", CONNECT_TIMEOUT_S);
		return;
	}

	/* ── 정적 IP 설정 ─── */
	if (wifi_set_static_ip(iface) != 0) {
		return;
	}

	LOG_INF("=== WiFi 준비 완료 ===");
	k_sem_give(&sem_wifi_ready);
}

/* ── WiFi 준비 대기 ──────────────────────────────────── */
int wifi_wait_ready(int timeout_ms)
{
	return k_sem_take(&sem_wifi_ready, K_MSEC(timeout_ms));
}

/* ── 태스크 시작 함수 ────────────────────────────────── */
static K_THREAD_STACK_DEFINE(wifi_stack, WIFI_STACK_SIZE);
static struct k_thread wifi_thread;

int wifi_task_start(void)
{
	k_tid_t tid = k_thread_create(&wifi_thread, wifi_stack,
				      WIFI_STACK_SIZE,
				      wifi_task, NULL, NULL, NULL,
				      WIFI_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(tid, "wifi_task");
	LOG_INF("WiFi 태스크 생성 완료");
	return 0;
}
