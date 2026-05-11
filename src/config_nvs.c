/*
 * SmartGateway — NVS 설정 저장 + 시리얼 콘솔 메뉴
 *
 * 부팅 순서:
 *   main() → config_nvs_load()  : Kconfig 기본값 로드 후 NVS 덮어쓰기
 *           → config_nvs_menu() : 15초 타임아웃 — 키 입력 시 설정 메뉴 진입
 */

#include "config_nvs.h"

#include <zephyr/kernel.h>
#include <zephyr/autoconf.h>
#include <zephyr/sys/util.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/console/console.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#if defined(CONFIG_NVS) && defined(CONFIG_FLASH) && defined(CONFIG_FLASH_MAP)
#define CFG_HAS_NVS 1
#else
#define CFG_HAS_NVS 0
#endif

#if defined(CONFIG_CONSOLE_SUBSYS) && defined(CONFIG_CONSOLE_GETCHAR)
#define CFG_HAS_CONSOLE_MENU 1
#else
#define CFG_HAS_CONSOLE_MENU 0
#endif

#define CFG_MENU_TRIGGER_TIMEOUT_S 15

/* ── NVS 스키마 버전 (구조체 변경 시 증가 → 구버전 NVS 자동 무시) ── */
/* 5: net_boot_mode 0/1만 (WiFi·ETH 두 모드, AUTO 제거) — 구 스키마(4 이하) 무효화 */
#define NVS_SCHEMA_VERSION 5U
#define KEY_SCHEMA_VER     0   /* key 0 */

/* ── NVS 키 ──────────────────────────────────────────────────── */
#define KEY_MASTER_CODE    1
#define KEY_ETH_IP         2
#define KEY_ETH_MASK       3
#define KEY_ETH_GW         4
#define KEY_ETH_TCP_IP     5
#define KEY_ETH_TCP_PORT   6
#define KEY_ETH_UDP_IP     7
#define KEY_ETH_UDP_PORT   8
#define KEY_WIFI_SSID      9
#define KEY_WIFI_PSK      10
#define KEY_WIFI_IP       11
#define KEY_WIFI_MASK     12
#define KEY_WIFI_GW       13
#define KEY_WIFI_TCP_IP   14
#define KEY_WIFI_TCP_PORT 15
#define KEY_WIFI_UDP_IP   16
#define KEY_WIFI_UDP_PORT 17
#define KEY_NET_BOOT_MODE 18
#define KEY_DEVICE_INDEX  19
#define KEY_LAST          KEY_DEVICE_INDEX

/* ── 전역 설정 구조체 ─────────────────────────────────────────── */
gw_config_t g_gw_config;

/* ── NVS 인스턴스 ────────────────────────────────────────────── */
#if CFG_HAS_NVS
static struct nvs_fs nvs;
#endif
static bool nvs_ok;
static void load_defaults(void);

#if CFG_HAS_CONSOLE_MENU
static void cfg_reset_to_defaults(bool erase_nvs)
{
	load_defaults();

	if (!erase_nvs || !nvs_ok) {
		return;
	}

#if CFG_HAS_NVS
	for (uint16_t key = KEY_SCHEMA_VER; key <= KEY_LAST; key++) {
		(void)nvs_delete(&nvs, key);
	}
#endif
}
#endif

static void nvs_init_fs(void)
{
#if CFG_HAS_NVS
	/* FRDM-MCXN947: w25q64 QSPI — frdm_mcxn947_mcxn947_cpu0.dtsi storage_partition */
	const struct device *flash_dev = FIXED_PARTITION_DEVICE(storage_partition);

	if (!device_is_ready(flash_dev)) {
		printf("[CFG] flash device not ready\n");
		return;
	}

	struct flash_pages_info pi;
	off_t part_off = FIXED_PARTITION_OFFSET(storage_partition);
	uint32_t sector_sz = 8192U;

	if (flash_get_page_info_by_offs(flash_dev, part_off, &pi) == 0) {
		sector_sz = (uint32_t)pi.size;
	}

	nvs.flash_device = flash_dev;
	nvs.offset       = part_off;
	nvs.sector_size  = (uint16_t)sector_sz;
	nvs.sector_count = (uint16_t)(FIXED_PARTITION_SIZE(storage_partition) / sector_sz);

	int r = nvs_mount(&nvs);

	nvs_ok = (r == 0);
	if (!nvs_ok) {
		printf("[CFG] NVS mount fail: %d\n", r);
	}
#else
	nvs_ok = false;
	printf("[CFG] NVS 비활성 빌드 — 기본값 모드\n");
#endif
}

/* ── Kconfig 기본값 ──────────────────────────────────────────── */
static void load_defaults(void)
{
	memset(&g_gw_config, 0, sizeof(g_gw_config));

	strncpy(g_gw_config.master_code, CONFIG_SMARTGATEWAY_LINE_ID,
		sizeof(g_gw_config.master_code) - 1);
#if defined(CONFIG_SMARTGATEWAY_PROTO_DEVICE_ID)
	g_gw_config.device_index =
		(uint16_t)MIN((unsigned int)CONFIG_SMARTGATEWAY_PROTO_DEVICE_ID, 65535U);
#else
	g_gw_config.device_index = 1U;
#endif

	/* ETH */
	strncpy(g_gw_config.eth_ip, CONFIG_SMARTGATEWAY_ETH_STATIC_IP,
		sizeof(g_gw_config.eth_ip) - 1);
	strncpy(g_gw_config.eth_netmask, CONFIG_SMARTGATEWAY_ETH_STATIC_NETMASK,
		sizeof(g_gw_config.eth_netmask) - 1);
	strncpy(g_gw_config.eth_gw, CONFIG_SMARTGATEWAY_ETH_STATIC_GW,
		sizeof(g_gw_config.eth_gw) - 1);
	strncpy(g_gw_config.eth_tcp_server_ip, CONFIG_SMARTGATEWAY_ETH_TCP_PEER_IP,
		sizeof(g_gw_config.eth_tcp_server_ip) - 1);
	g_gw_config.eth_tcp_server_port = CONFIG_SMARTGATEWAY_ETH_TCP_PEER_PORT;
	strncpy(g_gw_config.eth_udp_server_ip, CONFIG_SMARTGATEWAY_ETH_UDP_PEER_IP,
		sizeof(g_gw_config.eth_udp_server_ip) - 1);
	g_gw_config.eth_udp_server_port = CONFIG_SMARTGATEWAY_ETH_UDP_PEER_PORT;

	/* WiFi */
#if IS_ENABLED(CONFIG_SMARTGATEWAY_WIFI_ENABLE)
	strncpy(g_gw_config.wifi_ssid, CONFIG_SMARTGATEWAY_WIFI_SSID,
		sizeof(g_gw_config.wifi_ssid) - 1);
	strncpy(g_gw_config.wifi_psk, CONFIG_SMARTGATEWAY_WIFI_PSK,
		sizeof(g_gw_config.wifi_psk) - 1);
	strncpy(g_gw_config.wifi_ip, CONFIG_SMARTGATEWAY_WIFI_STATIC_IP,
		sizeof(g_gw_config.wifi_ip) - 1);
	strncpy(g_gw_config.wifi_netmask, CONFIG_SMARTGATEWAY_WIFI_STATIC_NETMASK,
		sizeof(g_gw_config.wifi_netmask) - 1);
	strncpy(g_gw_config.wifi_gw, CONFIG_SMARTGATEWAY_WIFI_STATIC_GW,
		sizeof(g_gw_config.wifi_gw) - 1);
	strncpy(g_gw_config.wifi_tcp_server_ip, CONFIG_SMARTGATEWAY_WIFI_TCP_PEER_IP,
		sizeof(g_gw_config.wifi_tcp_server_ip) - 1);
	g_gw_config.wifi_tcp_server_port = CONFIG_SMARTGATEWAY_WIFI_TCP_PEER_PORT;
	strncpy(g_gw_config.wifi_udp_server_ip, CONFIG_SMARTGATEWAY_WIFI_UDP_PEER_IP,
		sizeof(g_gw_config.wifi_udp_server_ip) - 1);
	g_gw_config.wifi_udp_server_port = CONFIG_SMARTGATEWAY_WIFI_UDP_PEER_PORT;
	/* strncpy는 꽉 찰 때 널 미보장 — WiFi 문자열 명시 종료 */
	g_gw_config.wifi_ssid[sizeof(g_gw_config.wifi_ssid) - 1] = '\0';
	g_gw_config.wifi_psk[sizeof(g_gw_config.wifi_psk) - 1] = '\0';
#endif

#if IS_ENABLED(CONFIG_SMARTGATEWAY_WIFI_ENABLE)
	{
		int d = CONFIG_SMARTGATEWAY_NET_BOOT_DEFAULT;

		if (d < 0 || d > 1) {
			d = 0;
		}
		g_gw_config.net_boot_mode = (uint8_t)d;
	}
#else
	g_gw_config.net_boot_mode = GW_NET_BOOT_ETH;
#endif
}

/* ── NVS 읽기 헬퍼 ───────────────────────────────────────────── */
static void nvs_rd_str(uint16_t key, char *dst, size_t max_len)
{
#if CFG_HAS_NVS
	/*
	 * 임시 버퍼로 읽어서 실제 내용이 있을 때만 dst에 복사한다.
	 * NVS에 빈 문자열(r==1, null 1바이트만 저장)이 있으면 load_defaults()가
	 * 채운 Kconfig 기본값을 덮어쓰지 않도록 한다.
	 */
	char tmp[68]; /* wifi_psk(65B) 포함 모든 필드 수용 */
	size_t rd = (max_len - 1U < sizeof(tmp) - 1U) ? max_len - 1U : sizeof(tmp) - 1U;
	ssize_t r = nvs_read(&nvs, key, tmp, rd);

	if (r > 1) { /* r==1: 빈 문자열, r<0: 키 없음 → 기본값 유지 */
		tmp[r] = '\0';
		memcpy(dst, tmp, (size_t)r + 1U);
	}
#else
	ARG_UNUSED(key);
	ARG_UNUSED(dst);
	ARG_UNUSED(max_len);
#endif
}

static void nvs_rd_u16(uint16_t key, uint16_t *val)
{
#if CFG_HAS_NVS
	uint16_t tmp;

	/* 0이 저장된 경우(WiFi 비활성 빌드에서 저장된 초기값) Kconfig 기본값 유지 */
	if (nvs_read(&nvs, key, &tmp, sizeof(tmp)) == (ssize_t)sizeof(tmp) && tmp != 0U) {
		*val = tmp;
	}
#else
	ARG_UNUSED(key);
	ARG_UNUSED(val);
#endif
}

static void nvs_rd_u16_allow_zero(uint16_t key, uint16_t *val)
{
#if CFG_HAS_NVS
	uint16_t tmp;

	if (nvs_read(&nvs, key, &tmp, sizeof(tmp)) == (ssize_t)sizeof(tmp)) {
		*val = tmp;
	}
#else
	ARG_UNUSED(key);
	ARG_UNUSED(val);
#endif
}

static void nvs_rd_u8_boot_mode(uint16_t key, uint8_t *val)
{
#if CFG_HAS_NVS
	uint8_t tmp;

	if (nvs_read(&nvs, key, &tmp, sizeof(tmp)) == (ssize_t)sizeof(tmp) &&
	    tmp <= GW_NET_BOOT_ETH) {
		*val = tmp;
	}
#else
	ARG_UNUSED(key);
	ARG_UNUSED(val);
#endif
}

/* ── 공개 API ────────────────────────────────────────────────── */
void config_nvs_load(void)
{
	load_defaults();
	nvs_init_fs();

	if (!nvs_ok) {
		printf("[CFG] NVS 없음 — Kconfig 기본값 사용\n");
		return;
	}

	/* 스키마 버전 확인: 불일치 시 구버전 NVS 무시하고 기본값 유지 */
	uint16_t stored_ver = 0;
	nvs_rd_u16(KEY_SCHEMA_VER, &stored_ver);
	if (stored_ver != NVS_SCHEMA_VERSION) {
		printf("[CFG] NVS 스키마 버전 불일치 (저장=%u, 현재=%u) — 기본값 사용, NVS 갱신\n",
		       stored_ver, NVS_SCHEMA_VERSION);
#if CFG_HAS_NVS
		/* 구버전 키 전체 삭제 */
		for (uint16_t k = 0; k <= KEY_LAST; k++) {
			(void)nvs_delete(&nvs, k);
		}
		uint16_t v = NVS_SCHEMA_VERSION;
		(void)nvs_write(&nvs, KEY_SCHEMA_VER, &v, sizeof(v));
#endif
		return;
	}

	nvs_rd_str(KEY_MASTER_CODE,  g_gw_config.master_code,        sizeof(g_gw_config.master_code));
	nvs_rd_u16_allow_zero(KEY_DEVICE_INDEX, &g_gw_config.device_index);
	nvs_rd_str(KEY_ETH_IP,       g_gw_config.eth_ip,             sizeof(g_gw_config.eth_ip));
	nvs_rd_str(KEY_ETH_MASK,     g_gw_config.eth_netmask,        sizeof(g_gw_config.eth_netmask));
	nvs_rd_str(KEY_ETH_GW,       g_gw_config.eth_gw,             sizeof(g_gw_config.eth_gw));
	nvs_rd_str(KEY_ETH_TCP_IP,   g_gw_config.eth_tcp_server_ip,  sizeof(g_gw_config.eth_tcp_server_ip));
	nvs_rd_u16(KEY_ETH_TCP_PORT, &g_gw_config.eth_tcp_server_port);
	nvs_rd_str(KEY_ETH_UDP_IP,   g_gw_config.eth_udp_server_ip,  sizeof(g_gw_config.eth_udp_server_ip));
	nvs_rd_u16(KEY_ETH_UDP_PORT, &g_gw_config.eth_udp_server_port);
	nvs_rd_str(KEY_WIFI_SSID,    g_gw_config.wifi_ssid,          sizeof(g_gw_config.wifi_ssid));
	nvs_rd_str(KEY_WIFI_PSK,     g_gw_config.wifi_psk,           sizeof(g_gw_config.wifi_psk));
	nvs_rd_str(KEY_WIFI_IP,      g_gw_config.wifi_ip,            sizeof(g_gw_config.wifi_ip));
	nvs_rd_str(KEY_WIFI_MASK,    g_gw_config.wifi_netmask,       sizeof(g_gw_config.wifi_netmask));
	nvs_rd_str(KEY_WIFI_GW,      g_gw_config.wifi_gw,            sizeof(g_gw_config.wifi_gw));
	nvs_rd_str(KEY_WIFI_TCP_IP,  g_gw_config.wifi_tcp_server_ip, sizeof(g_gw_config.wifi_tcp_server_ip));
	nvs_rd_u16(KEY_WIFI_TCP_PORT,&g_gw_config.wifi_tcp_server_port);
	nvs_rd_str(KEY_WIFI_UDP_IP,  g_gw_config.wifi_udp_server_ip, sizeof(g_gw_config.wifi_udp_server_ip));
	nvs_rd_u16(KEY_WIFI_UDP_PORT,&g_gw_config.wifi_udp_server_port);
	nvs_rd_u8_boot_mode(KEY_NET_BOOT_MODE, &g_gw_config.net_boot_mode);

#if !IS_ENABLED(CONFIG_SMARTGATEWAY_WIFI_ENABLE)
	g_gw_config.net_boot_mode = GW_NET_BOOT_ETH;
#else
	if (g_gw_config.net_boot_mode > GW_NET_BOOT_ETH) {
		g_gw_config.net_boot_mode = GW_NET_BOOT_WIFI;
	}
#endif

#if IS_ENABLED(CONFIG_SMARTGATEWAY_FORCE_ETH_BOOT_MODE)
	if (nvs_ok && g_gw_config.net_boot_mode != GW_NET_BOOT_ETH) {
		g_gw_config.net_boot_mode = GW_NET_BOOT_ETH;
		config_nvs_save_boot_mode();
		printf("[CFG] FORCE_ETH_BOOT: net_boot_mode -> ETH, NVS 저장됨\n");
	}
#elif IS_ENABLED(CONFIG_SMARTGATEWAY_FORCE_WIFI_BOOT_MODE) && IS_ENABLED(CONFIG_SMARTGATEWAY_WIFI_ENABLE)
	if (nvs_ok && g_gw_config.net_boot_mode != GW_NET_BOOT_WIFI) {
		g_gw_config.net_boot_mode = GW_NET_BOOT_WIFI;
		config_nvs_save_boot_mode();
		printf("[CFG] FORCE_WIFI_BOOT: net_boot_mode -> WiFi, NVS 저장됨\n");
	}
#endif

	printf("[CFG] config loaded OK\n");
}

void config_nvs_save(void)
{
	if (!nvs_ok) {
		printf("[CFG] NVS 초기화 안됨 — 저장 불가\n");
		return;
	}

#if CFG_HAS_NVS
#define WS(k, v) nvs_write(&nvs, (k), (v), strlen(v) + 1)
#define WU(k, v) nvs_write(&nvs, (k), &(v), sizeof(v))

	uint16_t schema_v = NVS_SCHEMA_VERSION;
	(void)nvs_write(&nvs, KEY_SCHEMA_VER, &schema_v, sizeof(schema_v));

	WS(KEY_MASTER_CODE,  g_gw_config.master_code);
	WU(KEY_DEVICE_INDEX, g_gw_config.device_index);
	WS(KEY_ETH_IP,       g_gw_config.eth_ip);
	WS(KEY_ETH_MASK,     g_gw_config.eth_netmask);
	WS(KEY_ETH_GW,       g_gw_config.eth_gw);
	WS(KEY_ETH_TCP_IP,   g_gw_config.eth_tcp_server_ip);
	WU(KEY_ETH_TCP_PORT, g_gw_config.eth_tcp_server_port);
	WS(KEY_ETH_UDP_IP,   g_gw_config.eth_udp_server_ip);
	WU(KEY_ETH_UDP_PORT, g_gw_config.eth_udp_server_port);
	WS(KEY_WIFI_SSID,    g_gw_config.wifi_ssid);
	WS(KEY_WIFI_PSK,     g_gw_config.wifi_psk);
	WS(KEY_WIFI_IP,      g_gw_config.wifi_ip);
	WS(KEY_WIFI_MASK,    g_gw_config.wifi_netmask);
	WS(KEY_WIFI_GW,      g_gw_config.wifi_gw);
	WS(KEY_WIFI_TCP_IP,  g_gw_config.wifi_tcp_server_ip);
	WU(KEY_WIFI_TCP_PORT,g_gw_config.wifi_tcp_server_port);
	WS(KEY_WIFI_UDP_IP,  g_gw_config.wifi_udp_server_ip);
	WU(KEY_WIFI_UDP_PORT,g_gw_config.wifi_udp_server_port);
	WU(KEY_NET_BOOT_MODE, g_gw_config.net_boot_mode);

#undef WS
#undef WU

	printf("[CFG] NVS 저장 완료\n");
#endif
}

void config_nvs_save_boot_mode(void)
{
#if CFG_HAS_NVS
	if (!nvs_ok) {
		printf("[CFG] NVS 초기화 안됨 — 부팅 모드 저장 생략\n");
		return;
	}

	uint8_t m = g_gw_config.net_boot_mode;

	(void)nvs_write(&nvs, KEY_NET_BOOT_MODE, &m, sizeof(m));
	{
		uint16_t schema_v = NVS_SCHEMA_VERSION;

		(void)nvs_write(&nvs, KEY_SCHEMA_VER, &schema_v, sizeof(schema_v));
	}
	printf("[CFG] NVS net_boot_mode=%u 저장 (다음 부팅부터 적용)\n", m);
#endif
}

/* ── 시리얼 메뉴 헬퍼 ────────────────────────────────────────── */
#if CFG_HAS_CONSOLE_MENU

static int read_line(char *buf, size_t size)
{
	size_t pos = 0;

	while (pos < size - 1U) {
		int c = console_getchar();

		if (c < 0) {
			continue;
		}
		if (c == '\r' || c == '\n') {
			buf[pos] = '\0';
			printf("\r\n");
			return (int)pos;
		}
		if ((c == 0x08 || c == 0x7F) && pos > 0) {
			pos--;
			printf("\b \b");
			continue;
		}
		if (c < 0x20) {
			continue;
		}
		buf[pos++] = (char)c;
		printf("%c", c);
	}
	buf[pos] = '\0';
	return (int)pos;
}

static bool parse_port(const char *s, uint16_t *out)
{
	unsigned long v = 0;
	const char *p = s;

	if (*p == '\0') {
		return false;
	}
	while (*p) {
		if (*p < '0' || *p > '9') {
			return false;
		}
		v = v * 10U + (unsigned)(*p - '0');
		if (v > 65535UL) {
			return false;
		}
		p++;
	}
	*out = (uint16_t)v;
	return true;
}

static const char *cfg_net_mode_line(void)
{
#if IS_ENABLED(CONFIG_SMARTGATEWAY_WIFI_ENABLE)
	if (g_gw_config.net_boot_mode == GW_NET_BOOT_ETH) {
		return "이더넷(1) — RJ45 프로파일만 사용";
	}
	return "WiFi(0) — 무선 프로파일만 사용";
#else
	return "이더넷만 (WiFi 빌드 아님)";
#endif
}

static void print_config(void)
{
	printf("\r\n");
	printf("==============================\r\n");
	printf(" SmartGateway NVS 설정\r\n");
	printf("==============================\r\n");
	printf(" 목적: 장치코드·인덱스·WiFi/ETH 주소·분할 모드를 플래시에 저장,\r\n");
	printf("       S 저장 후 재부팅하면 선택 모드(WiFi 또는 이더넷)로 동작.\r\n");
	printf(" [분할 부팅] 재부팅 후 사용할 스택 선택\r\n");
	printf("  현재 선택: %s\r\n", cfg_net_mode_line());
	printf("  N) 모드 숫자 입력 (0=WiFi  1=이더넷)\r\n");
#if IS_ENABLED(CONFIG_SMARTGATEWAY_WIFI_ENABLE)
	printf("  W) 빠름: 다음 부팅 WiFi(0)\r\n");
	printf("  L) 빠름: 다음 부팅 이더넷(1) — 아래 E 와 구분\r\n");
#endif
	printf("\r\n");
	printf(" 1) 장치 마스터 코드    : %s\r\n", g_gw_config.master_code);
	printf(" I) 장치 인덱스 (NVS 예약, UDP 무관, 0~65535): %u\r\n",
	       (unsigned)g_gw_config.device_index);

	printf("\r\n --- 이더넷 프로파일 (모드=1) ---\r\n");
	printf("     보드 10.108.x / PC 10.108.x 기본 권장\r\n");
	printf(" 2) ETH 보드 IP          : %s\r\n", g_gw_config.eth_ip);
	printf(" 3) ETH 마스크           : %s\r\n", g_gw_config.eth_netmask);
	printf(" 4) ETH 게이트웨이       : %s\r\n", g_gw_config.eth_gw);
	printf(" 5) ETH PC TCP IP        : %s\r\n", g_gw_config.eth_tcp_server_ip);
	printf(" 6) ETH TCP 포트         : %u\r\n", g_gw_config.eth_tcp_server_port);
	printf(" 7) ETH PC UDP IP        : %s\r\n", g_gw_config.eth_udp_server_ip);
	printf(" 8) ETH UDP 포트         : %u\r\n", g_gw_config.eth_udp_server_port);

#if IS_ENABLED(CONFIG_SMARTGATEWAY_WIFI_ENABLE)
	printf("\r\n --- WiFi 프로파일 (모드=0) ---\r\n");
	printf("     보드 192.168.88.x / PC 동일 대역 기본 권장\r\n");
	printf(" 9) WiFi SSID            : %s\r\n", g_gw_config.wifi_ssid);
	printf(" A) WiFi 비밀번호        : %s\r\n", g_gw_config.wifi_psk);
	printf(" B) WiFi 보드 IP         : %s\r\n", g_gw_config.wifi_ip);
	printf(" C) WiFi 마스크          : %s\r\n", g_gw_config.wifi_netmask);
	printf(" D) WiFi 게이트웨이      : %s\r\n", g_gw_config.wifi_gw);
	printf(" E) WiFi PC TCP IP       : %s\r\n", g_gw_config.wifi_tcp_server_ip);
	printf(" F) WiFi TCP 포트        : %u\r\n", g_gw_config.wifi_tcp_server_port);
	printf(" G) WiFi PC UDP IP       : %s\r\n", g_gw_config.wifi_udp_server_ip);
	printf(" H) WiFi UDP 포트        : %u\r\n", g_gw_config.wifi_udp_server_port);
#else
	printf("\r\n [WiFi 프로파일: 이 빌드에 비활성]\r\n");
#endif
	printf("------------------------------\r\n");
	printf(" R) Kconfig 초기값 복원 + NVS 삭제\r\n");
	printf(" S) NVS 저장 후 부팅 계속\r\n");
	printf(" Q) 저장 없이 부팅 계속\r\n");
	printf("==============================\r\n");
	printf(" 키 입력: ");
}

/** 장치코드·IP 등 문자열 — 빈 줄이면 유지 */
static void uart_prompt_str_keep(const char *label, char *dst, size_t dst_sz)
{
	char buf[72];

	printf("%s [%s]: ", label, dst);
	read_line(buf, sizeof(buf));
	if (buf[0] != '\0') {
		strncpy(dst, buf, dst_sz - 1);
		dst[dst_sz - 1] = '\0';
	}
}

#if IS_ENABLED(CONFIG_SMARTGATEWAY_WIFI_ENABLE)
static void uart_prompt_net_mode_keep(void)
{
	char buf[72];

	printf("분할 모드 (0=WiFi  1=이더넷) [%u]: ",
	       (unsigned)g_gw_config.net_boot_mode);
	read_line(buf, sizeof(buf));
	if (buf[0] == '0') {
		g_gw_config.net_boot_mode = GW_NET_BOOT_WIFI;
	} else if (buf[0] == '1') {
		g_gw_config.net_boot_mode = GW_NET_BOOT_ETH;
	}
}
#endif

/**
 * 부팅 후 UART로 한 줄씩 직접 입력 (Enter 만 = 유지).
 * PC TCP/UDP 피어 등은 M(상세 메뉴).
 */
static void uart_quick_setup_wizard(void)
{
	char buf[72];
	uint16_t u;

	printf("\r\n======== UART 설정 (직접 입력) ========\r\n");
	printf("각 항목: 새 값 후 Enter / 유지는 Enter 만\r\n\r\n");

#if IS_ENABLED(CONFIG_SMARTGATEWAY_WIFI_ENABLE)
	uart_prompt_net_mode_keep();
#else
	printf("[INFO] WiFi 비활성 빌드 — 분할 모드=이더넷만\r\n");
	g_gw_config.net_boot_mode = GW_NET_BOOT_ETH;
#endif

	uart_prompt_str_keep("장치코드", g_gw_config.master_code, sizeof(g_gw_config.master_code));

	printf("인덱스 (0~65535, NVS 예약) [%u]: ", (unsigned)g_gw_config.device_index);
	read_line(buf, sizeof(buf));
	if (buf[0] != '\0' && parse_port(buf, &u)) {
		g_gw_config.device_index = u;
	}

#if IS_ENABLED(CONFIG_SMARTGATEWAY_WIFI_ENABLE)
	uart_prompt_str_keep("WiFi SSID", g_gw_config.wifi_ssid, sizeof(g_gw_config.wifi_ssid));
	uart_prompt_str_keep("WiFi 비밀번호(PSK)", g_gw_config.wifi_psk, sizeof(g_gw_config.wifi_psk));
	uart_prompt_str_keep("WiFi 보드 IP", g_gw_config.wifi_ip, sizeof(g_gw_config.wifi_ip));
	uart_prompt_str_keep("WiFi 마스크", g_gw_config.wifi_netmask, sizeof(g_gw_config.wifi_netmask));
	uart_prompt_str_keep("WiFi 게이트웨이", g_gw_config.wifi_gw, sizeof(g_gw_config.wifi_gw));
#endif

	uart_prompt_str_keep("이더넷 보드 IP", g_gw_config.eth_ip, sizeof(g_gw_config.eth_ip));
	uart_prompt_str_keep("이더넷 마스크", g_gw_config.eth_netmask, sizeof(g_gw_config.eth_netmask));
	uart_prompt_str_keep("이더넷 게이트웨이", g_gw_config.eth_gw, sizeof(g_gw_config.eth_gw));

	printf("\r\n — PC TCP·UDP 피어·포트는 아래 M(상세)에서 —\r\n");
	printf("========================================\r\n");
}

/** 문자 단축키 메뉴 (상세): 피어·포트·복원 등. true = NVS 저장됨(부팅 계속). */
static bool cfg_menu_letter_loop(void)
{
	char buf[72];

	for (;;) {
		print_config();
		read_line(buf, sizeof(buf));
		if (buf[0] == '\0') {
			continue;
		}

		char sel = buf[0];

		if (sel >= 'a' && sel <= 'z') {
			sel = (char)(sel - 32);
		}

		if (sel == 'Q') {
			printf("[CFG] 상세 메뉴 종료 (저장 안 함)\r\n");
			return false;
		}
		if (sel == 'S') {
			config_nvs_save();
			return true;
		}
		if (sel == 'R') {
			cfg_reset_to_defaults(true);
			printf("[CFG] 초기값 복원 완료\r\n");
			continue;
		}
		if (sel == 'N') {
#if IS_ENABLED(CONFIG_SMARTGATEWAY_WIFI_ENABLE)
			printf(" 다음 부팅 모드 (0=WiFi  1=이더넷): ");
#else
			printf(" 이더넷만 가능 (1 입력): ");
#endif
			read_line(buf, sizeof(buf));
			if (buf[0] >= '0' && buf[0] <= '1') {
#if IS_ENABLED(CONFIG_SMARTGATEWAY_WIFI_ENABLE)
				g_gw_config.net_boot_mode = (uint8_t)(buf[0] - '0');
				printf(" → 설정됨: %s (S 로 플래시 저장)\r\n", cfg_net_mode_line());
#else
				g_gw_config.net_boot_mode = GW_NET_BOOT_ETH;
				printf(" [!] WiFi 비활성 빌드 → 이더넷(1)\r\n");
#endif
			} else {
				printf(" [!] 0 또는 1\r\n");
			}
			continue;
		}

#if IS_ENABLED(CONFIG_SMARTGATEWAY_WIFI_ENABLE)
		if (sel == 'W') {
			g_gw_config.net_boot_mode = GW_NET_BOOT_WIFI;
			printf(" → 다음 부팅 WiFi(0). 저장은 S\r\n");
			continue;
		}
		if (sel == 'L') {
			g_gw_config.net_boot_mode = GW_NET_BOOT_ETH;
			printf(" → 다음 부팅 이더넷(1). 저장은 S\r\n");
			continue;
		}
		if (sel == 'E' && buf[1] == '\0') {
			g_gw_config.net_boot_mode = GW_NET_BOOT_ETH;
			printf(" → 다음 부팅 이더넷(1). 저장은 S (단축키는 L 권장)\r\n");
			continue;
		}
#endif

		printf(" 새 값 입력 (Enter = 변경 없음): ");
		read_line(buf, sizeof(buf));
		if (buf[0] == '\0') {
			continue;
		}

		uint16_t port;

		switch (sel) {
		case '1':
			strncpy(g_gw_config.master_code, buf,
				sizeof(g_gw_config.master_code) - 1);
			g_gw_config.master_code[sizeof(g_gw_config.master_code) - 1] = '\0';
			break;
		case 'I':
			if (parse_port(buf, &port)) {
				g_gw_config.device_index = port;
			} else {
				printf(" [!] 0~65535 숫자\r\n");
			}
			break;
		case '2':
			strncpy(g_gw_config.eth_ip, buf, sizeof(g_gw_config.eth_ip) - 1);
			g_gw_config.eth_ip[sizeof(g_gw_config.eth_ip) - 1] = '\0';
			break;
		case '3':
			strncpy(g_gw_config.eth_netmask, buf,
				sizeof(g_gw_config.eth_netmask) - 1);
			g_gw_config.eth_netmask[sizeof(g_gw_config.eth_netmask) - 1] = '\0';
			break;
		case '4':
			strncpy(g_gw_config.eth_gw, buf, sizeof(g_gw_config.eth_gw) - 1);
			g_gw_config.eth_gw[sizeof(g_gw_config.eth_gw) - 1] = '\0';
			break;
		case '5':
			strncpy(g_gw_config.eth_tcp_server_ip, buf,
				sizeof(g_gw_config.eth_tcp_server_ip) - 1);
			g_gw_config.eth_tcp_server_ip[sizeof(g_gw_config.eth_tcp_server_ip) - 1] = '\0';
			break;
		case '6':
			if (parse_port(buf, &port)) {
				g_gw_config.eth_tcp_server_port = port;
			} else {
				printf(" [!] 잘못된 포트\r\n");
			}
			break;
		case '7':
			strncpy(g_gw_config.eth_udp_server_ip, buf,
				sizeof(g_gw_config.eth_udp_server_ip) - 1);
			g_gw_config.eth_udp_server_ip[sizeof(g_gw_config.eth_udp_server_ip) - 1] = '\0';
			break;
		case '8':
			if (parse_port(buf, &port)) {
				g_gw_config.eth_udp_server_port = port;
			} else {
				printf(" [!] 잘못된 포트\r\n");
			}
			break;
#if IS_ENABLED(CONFIG_SMARTGATEWAY_WIFI_ENABLE)
		case '9':
			strncpy(g_gw_config.wifi_ssid, buf,
				sizeof(g_gw_config.wifi_ssid) - 1);
			g_gw_config.wifi_ssid[sizeof(g_gw_config.wifi_ssid) - 1] = '\0';
			break;
		case 'A':
			strncpy(g_gw_config.wifi_psk, buf,
				sizeof(g_gw_config.wifi_psk) - 1);
			g_gw_config.wifi_psk[sizeof(g_gw_config.wifi_psk) - 1] = '\0';
			break;
		case 'B':
			strncpy(g_gw_config.wifi_ip, buf,
				sizeof(g_gw_config.wifi_ip) - 1);
			g_gw_config.wifi_ip[sizeof(g_gw_config.wifi_ip) - 1] = '\0';
			break;
		case 'C':
			strncpy(g_gw_config.wifi_netmask, buf,
				sizeof(g_gw_config.wifi_netmask) - 1);
			g_gw_config.wifi_netmask[sizeof(g_gw_config.wifi_netmask) - 1] = '\0';
			break;
		case 'D':
			strncpy(g_gw_config.wifi_gw, buf,
				sizeof(g_gw_config.wifi_gw) - 1);
			g_gw_config.wifi_gw[sizeof(g_gw_config.wifi_gw) - 1] = '\0';
			break;
		case 'E':
			strncpy(g_gw_config.wifi_tcp_server_ip, buf,
				sizeof(g_gw_config.wifi_tcp_server_ip) - 1);
			g_gw_config.wifi_tcp_server_ip[sizeof(g_gw_config.wifi_tcp_server_ip) - 1] = '\0';
			break;
		case 'F':
			if (parse_port(buf, &port)) {
				g_gw_config.wifi_tcp_server_port = port;
			} else {
				printf(" [!] 잘못된 포트\r\n");
			}
			break;
		case 'G':
			strncpy(g_gw_config.wifi_udp_server_ip, buf,
				sizeof(g_gw_config.wifi_udp_server_ip) - 1);
			g_gw_config.wifi_udp_server_ip[sizeof(g_gw_config.wifi_udp_server_ip) - 1] = '\0';
			break;
		case 'H':
			if (parse_port(buf, &port)) {
				g_gw_config.wifi_udp_server_port = port;
			} else {
				printf(" [!] 잘못된 포트\r\n");
			}
			break;
#else
		case '9': case 'A': case 'B': case 'C': case 'D':
		case 'E': case 'F': case 'G': case 'H':
			printf(" [!] WiFi 비활성 빌드 — 이 항목은 편집 불가\r\n");
			break;
#endif
		default:
			printf(" [!] 알 수 없는 항목\r\n");
			break;
		}
	}
}

static int cfg_wait_for_setup_key(void)
{
	int64_t deadline = k_uptime_get() + (CFG_MENU_TRIGGER_TIMEOUT_S * 1000);

	while (k_uptime_get() < deadline) {
		int c = console_getchar();

		if (c >= 0) {
			return c;
		}
		k_msleep(20);
	}
	return -1;
}

static void cfg_drain_trigger_line(void)
{
	int idle_ms = 0;

	while (idle_ms < 80) {
		int c = console_getchar();

		if (c < 0) {
			k_msleep(10);
			idle_ms += 10;
			continue;
		}
		idle_ms = 0;
		if (c == '\r' || c == '\n') {
			return;
		}
	}
}

void config_nvs_menu(void)
{
	console_init();

	printf("\r\n[CFG] %ds 안 아무 키나 누르면 UART에서 모드·코드·IP 등 직접 입력\r\n",
	       CFG_MENU_TRIGGER_TIMEOUT_S);

	int trigger = cfg_wait_for_setup_key();

	if (trigger < 0) {
		printf("[CFG] timeout — continuing with stored config\r\n");
		return;
	}
	printf("[CFG] key received: 0x%02x — UART 설정 시작\r\n",
	       (unsigned int)(trigger & 0xff));
	cfg_drain_trigger_line();

	uart_quick_setup_wizard();

	char buf[72];

	for (;;) {
		printf("\r\n-------- 저장 / 종료 --------\r\n");
		printf(" S) NVS 저장 후 부팅 계속\r\n");
		printf(" Q) 저장 없이 부팅 계속\r\n");
		printf(" M) 상세 메뉴 (PC TCP·UDP 피어·포트·단축키)\r\n");
		printf(" 선택: ");
		read_line(buf, sizeof(buf));
		if (buf[0] == '\0') {
			continue;
		}

		char sel = buf[0];
		if (sel >= 'a' && sel <= 'z') {
			sel = (char)(sel - 32);
		}

		if (sel == 'S') {
			config_nvs_save();
			break;
		}
		if (sel == 'Q') {
			printf("[CFG] 저장 없이 계속\r\n");
			break;
		}
		if (sel == 'M') {
			if (cfg_menu_letter_loop()) {
				break;
			}
			continue;
		}

		printf(" [!] S / Q / M 만\r\n");
	}
}
#else
void config_nvs_menu(void)
{
	printf("[CFG] 콘솔 메뉴 비활성 빌드 — 설정 메뉴 건너뜀\n");
}
#endif
