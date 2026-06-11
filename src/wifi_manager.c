/*
 * SmartGateway — WiFi (NVS g_gw_config + netmgr용 동기 API)
 *
 * wifi_task_start / wifi_wait_ready: 레거시 단일 부팅 태스크 경로
 * wifi_mgr_init / wifi_connect_once / wifi_is_ready / wifi_force_disconnect:
 *   network_manager.c 분할 부팅 오케스트레이션용
 */

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/logging/log.h>
#include <zephyr/autoconf.h>
#include <string.h>

#include "wifi_manager.h"
#include "config_nvs.h"

LOG_MODULE_REGISTER(wifi_mgr, LOG_LEVEL_INF);

#define WIFI_STACK_SIZE   4096
#define WIFI_PRIORITY     5

#define CONNECT_TIMEOUT_S 30

static K_SEM_DEFINE(sem_connected, 0, 1);
static K_SEM_DEFINE(sem_wifi_ready, 0, 1);

static struct net_mgmt_event_callback wifi_cb;
static bool                          s_mgr_inited;

static void on_wifi_event(struct net_mgmt_event_callback *cb,
			  uint64_t event, struct net_if *iface)
{
	if (event == NET_EVENT_WIFI_CONNECT_RESULT) {
		const struct wifi_status *st =
			(const struct wifi_status *)cb->info;
		if (st->status == 0) {
			LOG_INF("WiFi connected: SSID=%s", g_gw_config.wifi_ssid);
			k_sem_give(&sem_connected);
		} else {
			LOG_ERR("WiFi connect failed (status=%d)", st->status);
		}
	} else if (event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
		LOG_WRN("WiFi disconnected");
	}
}

static void wifi_fill_connect_params(struct wifi_connect_req_params *params)
{
	memset(params, 0, sizeof(*params));

	params->ssid        = g_gw_config.wifi_ssid;
	params->ssid_length = strlen(g_gw_config.wifi_ssid);
	params->channel     = WIFI_CHANNEL_ANY;
#if IS_ENABLED(CONFIG_SMARTGATEWAY_WIFI_SCAN_BAND_2_4_GHZ)
	params->band        = WIFI_FREQ_BAND_2_4_GHZ;
#elif IS_ENABLED(CONFIG_SMARTGATEWAY_WIFI_SCAN_BAND_5_GHZ)
	params->band        = WIFI_FREQ_BAND_5_GHZ;
#else
	params->band        = WIFI_FREQ_BAND_UNKNOWN;
#endif
	params->mfp         = WIFI_MFP_OPTIONAL;

#if IS_ENABLED(CONFIG_SMARTGATEWAY_WIFI_SECURITY_WPA3)
	params->sae_password        = g_gw_config.wifi_psk;
	params->sae_password_length = strlen(g_gw_config.wifi_psk);
	params->security            = WIFI_SECURITY_TYPE_SAE_AUTO;
#elif IS_ENABLED(CONFIG_SMARTGATEWAY_WIFI_SECURITY_WPA_AUTO)
	params->psk                 = g_gw_config.wifi_psk;
	params->psk_length          = strlen(g_gw_config.wifi_psk);
	params->sae_password        = g_gw_config.wifi_psk;
	params->sae_password_length = strlen(g_gw_config.wifi_psk);
	params->security            = WIFI_SECURITY_TYPE_WPA_AUTO_PERSONAL;
#elif IS_ENABLED(CONFIG_SMARTGATEWAY_WIFI_SECURITY_WPA2)
	params->psk        = g_gw_config.wifi_psk;
	params->psk_length = strlen(g_gw_config.wifi_psk);
	params->security   = WIFI_SECURITY_TYPE_PSK;
#else
	params->security = WIFI_SECURITY_TYPE_NONE;
#endif
}

static int wifi_set_static_ip(struct net_if *iface)
{
	struct in_addr addr, mask, gw;

	if (net_addr_pton(AF_INET, g_gw_config.wifi_ip, &addr) ||
	    net_addr_pton(AF_INET, g_gw_config.wifi_netmask, &mask) ||
	    net_addr_pton(AF_INET, g_gw_config.wifi_gw, &gw)) {
		LOG_ERR("WiFi IP parse fail");
		return -EINVAL;
	}

	if (!net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0)) {
		LOG_ERR("WiFi IP add fail");
		return -ENODEV;
	}

	net_if_ipv4_set_netmask_by_addr(iface, &addr, &mask);
	net_if_ipv4_set_gw(iface, &gw);
	net_if_set_default(iface);

	/* 기본 라우터 등록: 외부 트래픽(DNS·인터넷)을 GW로 포워딩 */
	if (!net_if_ipv4_router_add(iface, &gw, true, 0)) {
		LOG_ERR("WiFi router add failed");
	}

	LOG_INF("WiFi static IP %s / %s gw %s",
		g_gw_config.wifi_ip, g_gw_config.wifi_netmask, g_gw_config.wifi_gw);
	return 0;
}

int wifi_mgr_init(void)
{
	struct net_if *iface;

	if (s_mgr_inited) {
		return 0;
	}

	iface = net_if_get_first_wifi();
	if (!iface) {
		LOG_ERR("no WiFi iface");
		return -ENODEV;
	}

	net_mgmt_init_event_callback(&wifi_cb, on_wifi_event,
				     NET_EVENT_WIFI_CONNECT_RESULT |
					     NET_EVENT_WIFI_DISCONNECT_RESULT);
	net_mgmt_add_event_callback(&wifi_cb);
	k_sleep(K_SECONDS(2));
	s_mgr_inited = true;
	return 0;
}

int wifi_connect_once(int timeout_ms)
{
	struct net_if *iface;
	static struct wifi_connect_req_params params;

	if (wifi_mgr_init() != 0) {
		return -ENODEV;
	}

	iface = net_if_get_first_wifi();
	if (!iface) {
		return -ENODEV;
	}

	k_sem_reset(&sem_connected);
	wifi_fill_connect_params(&params);

	LOG_INF("AP connect '%s' sec=%d ...", g_gw_config.wifi_ssid, params.security);

	if (net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params))) {
		LOG_ERR("NET_REQUEST_WIFI_CONNECT fail");
		return -EIO;
	}

	if (k_sem_take(&sem_connected, K_MSEC(timeout_ms)) != 0) {
		LOG_ERR("WiFi assoc/IP timeout (%d ms)", timeout_ms);
		return -ETIMEDOUT;
	}

	return wifi_set_static_ip(iface);
}

bool wifi_is_ready(void)
{
	struct net_if *iface = net_if_get_first_wifi();
	struct in_addr      want;
	struct net_if        *found = iface;

	if (!iface || !net_if_is_up(iface)) {
		return false;
	}
	if (net_addr_pton(AF_INET, g_gw_config.wifi_ip, &want) != 0) {
		return false;
	}
	return net_if_ipv4_addr_lookup(&want, &found) != NULL;
}

void wifi_force_disconnect(void)
{
	struct net_if *iface = net_if_get_first_wifi();

	if (iface) {
		(void)net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
	}
}

/* ── 레거시: 단일 WiFi 태스크 (wifi_task_start) ─────────────────── */

static void wifi_task(void *p1, void *p2, void *p3)
{
	struct net_if *iface;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	LOG_INF("wifi_task");

	iface = net_if_get_first_wifi();
	if (!iface) {
		LOG_ERR("no WiFi iface");
		return;
	}

	if (wifi_mgr_init() != 0) {
		return;
	}

	static struct wifi_connect_req_params params;

	wifi_fill_connect_params(&params);

	LOG_INF("AP connect '%s' ...", g_gw_config.wifi_ssid);

	if (net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params))) {
		LOG_ERR("connect req fail");
		return;
	}

	if (k_sem_take(&sem_connected, K_SECONDS(CONNECT_TIMEOUT_S)) != 0) {
		LOG_ERR("AP timeout");
		return;
	}

	if (wifi_set_static_ip(iface) != 0) {
		return;
	}

	LOG_INF("WiFi ready");
	k_sem_give(&sem_wifi_ready);
}

int wifi_wait_ready(int timeout_ms)
{
	return k_sem_take(&sem_wifi_ready, K_MSEC(timeout_ms));
}

static K_THREAD_STACK_DEFINE(wifi_stack, WIFI_STACK_SIZE);
static struct k_thread wifi_thread;

int wifi_task_start(void)
{
	k_tid_t tid = k_thread_create(&wifi_thread, wifi_stack,
				      WIFI_STACK_SIZE,
				      wifi_task, NULL, NULL, NULL,
				      WIFI_PRIORITY, 0, K_NO_WAIT);

	if (tid == NULL) {
		return -1;
	}
	k_thread_name_set(tid, "wifi_task");
	return 0;
}
