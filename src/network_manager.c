/*
 * SmartGateway — Network Manager
 * NVS net_boot_mode: 0=WiFi 모드, 1=이더넷 모드 (AUTO·동일 부팅 폴백 없음).
 *   WiFi 모드: 이더넷 관리 DOWN 후 WiFi만 시도. 전부 실패 시 NVS=이더넷, 냉 재부팅.
 *   이더넷 모드: WiFi 단계 생략, 유선만.
 *
 * CONFIG_SMARTGATEWAY_WIFI_ENABLE=n 이면 이더넷만이며 WiFi 코드는 제외된다.
 */

#include "network_manager.h"
#include "config_nvs.h"

#include <zephyr/autoconf.h>
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <stdio.h>
#include <string.h>

#if IS_ENABLED(CONFIG_SMARTGATEWAY_WIFI_ENABLE)
#include "wifi_manager.h"
#include <zephyr/net/wifi_mgmt.h>
#endif

LOG_MODULE_REGISTER(net_mgr, LOG_LEVEL_INF);

#define NETMGR_STACK       7168
#define NETMGR_PRIORITY    5

#define WIFI_MAX_RETRIES   3
#define WIFI_CONNECT_TO_MS 30000

#define ETH_MAX_RETRIES    3
#define ETH_LINK_WAIT_MS   15000
#define ETH_LINK_POLL_MS   500
#define ETH_DOWN_WAIT_MS   2000

/* WiFi+ETH 모두 실패 시 다음 라운드까지 */
#define NETMGR_CYCLE_RETRY_S 5

K_THREAD_STACK_DEFINE(s_netmgr_stack, NETMGR_STACK);
static struct k_thread      s_netmgr_thr;
static K_SEM_DEFINE(s_sem_ready, 0, 1);

static volatile bool           s_ready;
static volatile netmgr_iface_t s_active;
static char                    s_local_ip[16];

/* ── 공개 API ──────────────────────────────────────────────────── */

bool netmgr_is_ready(void)
{
	return s_ready;
}

netmgr_iface_t netmgr_active_iface(void)
{
	return s_active;
}

const char *netmgr_active_iface_label(void)
{
	switch (s_active) {
	case NETMGR_IFACE_WIFI:
		return "WiFi";
	case NETMGR_IFACE_ETH:
		return "ETH";
	default:
		return "(none)";
	}
}

const char *netmgr_local_ip(void)
{
	return s_local_ip;
}

int netmgr_wait_ready(int timeout_ms)
{
	if (s_ready) {
		return 0;
	}
	return (k_sem_take(&s_sem_ready, K_MSEC(timeout_ms)) == 0) ? 0 : -1;
}

const char *netmgr_tcp_peer_ip(void)
{
	if (s_active == NETMGR_IFACE_WIFI) {
		return g_gw_config.wifi_tcp_server_ip;
	}
	if (s_active == NETMGR_IFACE_ETH) {
		return g_gw_config.eth_tcp_server_ip;
	}
	return "";
}

uint16_t netmgr_tcp_peer_port(void)
{
	if (s_active == NETMGR_IFACE_WIFI) {
		return g_gw_config.wifi_tcp_server_port;
	}
	if (s_active == NETMGR_IFACE_ETH) {
		return g_gw_config.eth_tcp_server_port;
	}
	return 0U;
}

const char *netmgr_udp_peer_ip(void)
{
	if (s_active == NETMGR_IFACE_WIFI) {
		return g_gw_config.wifi_udp_server_ip;
	}
	if (s_active == NETMGR_IFACE_ETH) {
		return g_gw_config.eth_udp_server_ip;
	}
	return "";
}

uint16_t netmgr_udp_peer_port(void)
{
	if (s_active == NETMGR_IFACE_WIFI) {
		return g_gw_config.wifi_udp_server_port;
	}
	if (s_active == NETMGR_IFACE_ETH) {
		return g_gw_config.eth_udp_server_port;
	}
	return 0U;
}

/* ── 내부 상태 전환 ────────────────────────────────────────────── */

static void set_active(netmgr_iface_t iface, const char *local_ip)
{
	strncpy(s_local_ip, local_ip, sizeof(s_local_ip) - 1);
	s_local_ip[sizeof(s_local_ip) - 1] = '\0';
	s_active = iface;
	s_ready  = true;
	k_sem_give(&s_sem_ready);
}

static void clear_active(void)
{
	s_ready       = false;
	s_active      = NETMGR_IFACE_NONE;
	s_local_ip[0] = '\0';
	k_sem_reset(&s_sem_ready);
}

/* ── ETH 헬퍼 ──────────────────────────────────────────────────── */

/*
 * Ethernet L2 타입 인터페이스만 반환한다.
 * loopback(DUMMY L2)이나 WiFi 인터페이스를 잘못 반환하지 않도록
 * net_if_l2() == ETHERNET 으로 필터링한다.
 */
static struct net_if *get_eth_iface(void)
{
	struct net_if *wifi = NULL;

#if IS_ENABLED(CONFIG_SMARTGATEWAY_WIFI_ENABLE)
	wifi = net_if_get_first_wifi();
#endif

	for (int idx = 1; idx <= 8; idx++) {
		struct net_if *iface = net_if_get_by_index(idx);

		if (!iface) {
			break;
		}
		if (iface == wifi) {
			continue;
		}
		/* Ethernet L2 인터페이스만 선택 (loopback 제외) */
		if (net_if_l2(iface) == &NET_L2_GET_NAME(ETHERNET)) {
			return iface;
		}
	}
	return NULL;
}

/*
 * 이더넷 “케이블 링크” 판단: PHY carrier(LOWER_UP) + 관리자 UP.
 * net_if_is_up() 은 NET_IF_RUNNING 까지 요구해, 폴백 직후 RUNNING 지연 시
 * 링크가 영구 미검출처럼 보일 수 있다.
 */
static bool eth_link_is_up(struct net_if *iface)
{
	return net_if_is_admin_up(iface) &&
	       net_if_flag_is_set(iface, NET_IF_LOWER_UP);
}

static int eth_set_static_ip(struct net_if *iface)
{
	struct in_addr addr, mask, gw;

	if (net_addr_pton(AF_INET, g_gw_config.eth_ip,      &addr) ||
	    net_addr_pton(AF_INET, g_gw_config.eth_netmask,  &mask) ||
	    net_addr_pton(AF_INET, g_gw_config.eth_gw,       &gw)) {
		printf("[NETMGR] ETH IP parse failed (ip=%s mask=%s gw=%s)\n",
		       g_gw_config.eth_ip, g_gw_config.eth_netmask, g_gw_config.eth_gw);
		return -EINVAL;
	}

	net_dhcpv4_stop(iface);

	/* 기존 주소 제거 후 재할당 */
	struct in_addr old;

	if (net_addr_pton(AF_INET, g_gw_config.eth_ip, &old) == 0) {
		net_if_ipv4_addr_rm(iface, &old);
	}

	if (!net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0)) {
		printf("[NETMGR] ETH IP add failed (net_if_ipv4_addr_add NULL)\n");
		return -ENODEV;
	}
	net_if_ipv4_set_netmask_by_addr(iface, &addr, &mask);
	net_if_ipv4_set_gw(iface, &gw);
	net_if_set_default(iface);

	printf("[NETMGR] ETH static IP OK: %s / %s  GW=%s\n",
	       g_gw_config.eth_ip, g_gw_config.eth_netmask, g_gw_config.eth_gw);
	return 0;
}

static void eth_clear_ip(struct net_if *iface)
{
	struct in_addr addr;

	if (net_addr_pton(AF_INET, g_gw_config.eth_ip, &addr) == 0) {
		net_if_ipv4_addr_rm(iface, &addr);
	}
}

static void eth_force_admin_down(struct net_if *iface)
{
	int waited = 0;

	if (!iface) {
		return;
	}

	if (net_if_is_admin_up(iface)) {
		net_if_down(iface);
	}
	/*
	 * net_if_carrier_off() 는 앱에서 부르면 PHY 실제 링크와 스택 carrier 상태가
	 * 어긋나 WiFi 실패 후 ETH 폴백에서 LOWER_UP 이 영구적으로 0인 것처럼
	 * 보일 수 있다. ENET 수신 스팸은 CONFIG_ETHERNET_LOG_LEVEL_ERR 로만 줄인다.
	 */
	while (net_if_is_admin_up(iface) && waited < ETH_DOWN_WAIT_MS) {
		k_msleep(100);
		waited += 100;
	}
	printf("[NETMGR] ETH down: admin_up=%d running=%d carrier=%d (wait=%dms)\n",
	       net_if_is_admin_up(iface),
	       net_if_flag_is_set(iface, NET_IF_RUNNING),
	       net_if_flag_is_set(iface, NET_IF_LOWER_UP),
	       waited);
}

/* ── WiFi 시도 ─────────────────────────────────────────────────── */

#if IS_ENABLED(CONFIG_SMARTGATEWAY_WIFI_ENABLE)
static bool try_wifi(void)
{
#if CONFIG_SMARTGATEWAY_DEFER_WIFI_CONNECT_MS > 0
	printf("[NETMGR] WiFi defer %d ms (spread RAM/crypto peak)\n",
	       CONFIG_SMARTGATEWAY_DEFER_WIFI_CONNECT_MS);
	k_msleep(CONFIG_SMARTGATEWAY_DEFER_WIFI_CONNECT_MS);
#endif

	printf("[NETMGR] ---- WiFi connect tries (max %d) ----\n", WIFI_MAX_RETRIES);

	if (wifi_mgr_init() != 0) {
		printf("[NETMGR] WiFi mgr init failed\n");
		return false;
	}

	for (int i = 0; i < WIFI_MAX_RETRIES; i++) {
		printf("[NETMGR] WiFi try %d/%d  SSID=%s\n",
		       i + 1, WIFI_MAX_RETRIES, g_gw_config.wifi_ssid);

		if (wifi_connect_once(WIFI_CONNECT_TO_MS) == 0) {
			printf("[NETMGR] WiFi OK  IP=%s\n", g_gw_config.wifi_ip);
			return true;
		}
		printf("[NETMGR] WiFi fail %d/%d\n", i + 1, WIFI_MAX_RETRIES);

		if (i < WIFI_MAX_RETRIES - 1) {
			printf("[NETMGR] retry in 3s...\n");
			k_sleep(K_SECONDS(3));
		}
	}

	printf("[NETMGR] WiFi all %d tries failed (WiFi 모드 → NVS=이더넷, 냉 재부팅)\n",
	       WIFI_MAX_RETRIES);
	return false;
}

static void wifi_connected_service_loop(void)
{
	set_active(NETMGR_IFACE_WIFI, g_gw_config.wifi_ip);
	printf("[NETMGR] === WiFi ACTIVE  IP=%s  TCP->%s:%u  UDP->%s:%u ===\n",
	       g_gw_config.wifi_ip,
	       g_gw_config.wifi_tcp_server_ip,
	       g_gw_config.wifi_tcp_server_port,
	       g_gw_config.wifi_udp_server_ip,
	       g_gw_config.wifi_udp_server_port);

	while (wifi_is_ready()) {
		k_sleep(K_SECONDS(1));
	}
	printf("[NETMGR] WiFi lost → 다음 사이클에서 재시도\n");
	clear_active();
	wifi_force_disconnect();
	k_msleep(300);
}
#endif /* CONFIG_SMARTGATEWAY_WIFI_ENABLE */

/* ── ETH 시도 ─────────────────────────────────────────────────── */

static bool try_eth(void)
{
	struct net_if *iface = get_eth_iface();

	printf("[NETMGR] ---- ETH connect tries (max %d) ----\n", ETH_MAX_RETRIES);

	if (!iface) {
		printf("[NETMGR] ETH iface missing (get_eth_iface=NULL)\n");
		return false;
	}

	printf("[NETMGR] ETH iface OK  planned board IP=%s\n",
	       g_gw_config.eth_ip);

	for (int i = 0; i < ETH_MAX_RETRIES; i++) {
		bool admin_up = net_if_is_admin_up(iface);
		bool carrier  = net_if_flag_is_set(iface, NET_IF_LOWER_UP);

		printf("[NETMGR] ETH link wait %d/%d  admin_up=%d carrier=%d running=%d\n",
		       i + 1, ETH_MAX_RETRIES, admin_up, carrier,
		       net_if_flag_is_set(iface, NET_IF_RUNNING));

		int elapsed = 0;

		while (elapsed < ETH_LINK_WAIT_MS) {
			if (eth_link_is_up(iface)) {
				break;
			}
			k_msleep(ETH_LINK_POLL_MS);
			elapsed += ETH_LINK_POLL_MS;

			/* 5초마다 상태 출력 */
			if (elapsed % 5000 == 0) {
				printf("[NETMGR]   ... waiting %ds  admin_up=%d carrier=%d running=%d\n",
				       elapsed / 1000,
				       net_if_is_admin_up(iface),
				       net_if_flag_is_set(iface, NET_IF_LOWER_UP),
				       net_if_flag_is_set(iface, NET_IF_RUNNING));
			}
		}

		if (!eth_link_is_up(iface)) {
			printf("[NETMGR] ETH no link %d/%d (check cable)\n",
			       i + 1, ETH_MAX_RETRIES);
			continue;
		}

		printf("[NETMGR] ETH link UP, assigning IP\n");

		if (eth_set_static_ip(iface) == 0) {
			printf("[NETMGR] === ETH ready ===\n");
			return true;
		}

		printf("[NETMGR] ETH IP assign failed, retry\n");
		eth_clear_ip(iface);
	}

	printf("[NETMGR] ETH all %d tries failed\n", ETH_MAX_RETRIES);
	return false;
}

/* 이더넷 한 사이클: 링크 대기 → 정적 IP → 세션 유지(링크 끊기면 정리). */
static bool run_eth_phase(void)
{
	struct net_if *eth_iface = get_eth_iface();

	printf("[NETMGR] Phase B: Ethernet (max %d tries)\n", ETH_MAX_RETRIES);

	if (eth_iface && !net_if_is_admin_up(eth_iface)) {
		printf("[NETMGR] ETH admin-up\n");
		net_if_up(eth_iface);
		k_msleep(500);
	} else if (eth_iface && !net_if_is_up(eth_iface)) {
		printf("[NETMGR] ETH resume\n");
		(void)net_if_up(eth_iface);
		k_msleep(300);
	}

	if (!try_eth()) {
		printf("[NETMGR] Phase B: Ethernet did not come up usable\n");
		return false;
	}

	set_active(NETMGR_IFACE_ETH, g_gw_config.eth_ip);
	printf("[NETMGR] === ETH ACTIVE  IP=%s  TCP->%s:%u  UDP->%s:%u ===\n",
	       g_gw_config.eth_ip,
	       g_gw_config.eth_tcp_server_ip,
	       g_gw_config.eth_tcp_server_port,
	       g_gw_config.eth_udp_server_ip,
	       g_gw_config.eth_udp_server_port);

	while (eth_iface && eth_link_is_up(eth_iface)) {
		k_sleep(K_SECONDS(1));
	}
	printf("[NETMGR] ETH link lost → next cycle\n");
	clear_active();
	if (eth_iface) {
		eth_clear_ip(eth_iface);
	}
	return true;
}

/* ── 메인 태스크 ────────────────────────────────────────────────── */

static void netmgr_task(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	printf("[NETMGR] netmgr task start (모드: NVS 0=WiFi 1=이더넷)\n");

	unsigned int cycle = 0U;

	for (;;) {
		cycle++;

#if !IS_ENABLED(CONFIG_SMARTGATEWAY_WIFI_ENABLE)
		printf("[NETMGR] ---------- cycle %u (이더넷 전용 빌드) ----------\n", cycle);
		{
			bool ok = run_eth_phase();

			if (!ok) {
				printf("[NETMGR] 이더넷: 준비 안 됨 → %ds 후 재시도\n",
				       NETMGR_CYCLE_RETRY_S);
			}
		}
		k_sleep(K_SECONDS(NETMGR_CYCLE_RETRY_S));
		continue;
#endif

#if IS_ENABLED(CONFIG_SMARTGATEWAY_WIFI_ENABLE)
		uint8_t boot_mode = g_gw_config.net_boot_mode;

		if (boot_mode > GW_NET_BOOT_ETH) {
			printf("[NETMGR] net_boot_mode=%u 손상 → WiFi(0)로 처리\n",
			       (unsigned int)boot_mode);
			boot_mode = GW_NET_BOOT_WIFI;
			g_gw_config.net_boot_mode = GW_NET_BOOT_WIFI;
		}

		if (boot_mode == GW_NET_BOOT_ETH) {
			bool ok;

			printf("[NETMGR] ---------- cycle %u (이더넷 모드) ----------\n", cycle);
			ok = run_eth_phase();
			if (!ok) {
				printf("[NETMGR] 이더넷 모드: 준비 안 됨 → NVS=WiFi, 냉 재부팅\n");
				g_gw_config.net_boot_mode = GW_NET_BOOT_WIFI;
				config_nvs_save_boot_mode();
				sys_reboot(SYS_REBOOT_COLD);
			}
			k_sleep(K_SECONDS(NETMGR_CYCLE_RETRY_S));
			continue;
		}

		printf("[NETMGR] ---------- cycle %u (WiFi 모드) ----------\n", cycle);
		{
			struct net_if *eth_pre = get_eth_iface();

			if (eth_pre) {
				printf("[NETMGR] 이더넷 admin-down 후 WiFi (%d회 시도)\n",
				       WIFI_MAX_RETRIES);
				eth_force_admin_down(eth_pre);
			}
		}

		if (!try_wifi()) {
			g_gw_config.net_boot_mode = GW_NET_BOOT_ETH;
			config_nvs_save_boot_mode();
			sys_reboot(SYS_REBOOT_COLD);
		}

		wifi_connected_service_loop();
		k_sleep(K_SECONDS(NETMGR_CYCLE_RETRY_S));
#endif
	}
}

int netmgr_start(void)
{
	printf("[NETMGR] netmgr start\n");
#if !IS_ENABLED(CONFIG_SMARTGATEWAY_WIFI_ENABLE) || \
	!IS_ENABLED(CONFIG_SMARTGATEWAY_WIFI_TEST_NO_ETH)
	printf("[NETMGR] ETH cfg: IP=%s  mask=%s  GW=%s\n",
	       g_gw_config.eth_ip, g_gw_config.eth_netmask, g_gw_config.eth_gw);
	printf("[NETMGR] ETH TCP peer: %s:%u\n",
	       g_gw_config.eth_tcp_server_ip, g_gw_config.eth_tcp_server_port);
	printf("[NETMGR] ETH UDP peer: %s:%u\n",
	       g_gw_config.eth_udp_server_ip, g_gw_config.eth_udp_server_port);
#else
	printf("[NETMGR] ETH: skipped (CONFIG_SMARTGATEWAY_WIFI_TEST_NO_ETH=y)\n");
#endif
#if IS_ENABLED(CONFIG_SMARTGATEWAY_WIFI_ENABLE)
	printf("[NETMGR] WiFi cfg: SSID=%s  IP=%s\n",
	       g_gw_config.wifi_ssid, g_gw_config.wifi_ip);
	printf("[NETMGR] WiFi TCP peer: %s:%u\n",
	       g_gw_config.wifi_tcp_server_ip, g_gw_config.wifi_tcp_server_port);
	printf("[NETMGR] WiFi UDP peer: %s:%u\n",
	       g_gw_config.wifi_udp_server_ip, g_gw_config.wifi_udp_server_port);
	{
		struct net_if *eth = get_eth_iface();

		printf("[NETMGR] Ethernet (board ENET): %s\n",
		       eth ? "driver present in DT" : "none — OK for wifi_test_no_eth overlay");
	}
#else
	printf("[NETMGR] WiFi: disabled (ETH-only build)\n");
#endif
#if IS_ENABLED(CONFIG_SMARTGATEWAY_WIFI_ENABLE)
	printf("[NETMGR] NVS 네트워크 모드: %u (0=WiFi 1=이더넷)\n",
	       (unsigned int)g_gw_config.net_boot_mode);
#endif

	k_tid_t t = k_thread_create(&s_netmgr_thr, s_netmgr_stack,
				     K_THREAD_STACK_SIZEOF(s_netmgr_stack),
				     netmgr_task, NULL, NULL, NULL,
				     NETMGR_PRIORITY, 0, K_NO_WAIT);

	if (t == NULL) {
		printf("[NETMGR] thread create failed\n");
		return -1;
	}
	k_thread_name_set(t, "netmgr");
	return 0;
}
