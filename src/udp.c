/*
 * Smart Gateway — UDP 전송/수신 태스크
 *
 * 관련 Kconfig (요약):
 *   WIFI_ENABLE=y → SMARTGATEWAY_WIFI_UDP_PEER_IP / _PEER_PORT / _BIND_PORT 사용
 *   WIFI_ENABLE=n → SMARTGATEWAY_ETH_UDP_PEER_IP  / _PEER_PORT / _BIND_PORT 사용
 *   CONFIG_SMARTGATEWAY_UDP_TEST_MODE — MessagePack 페이로드 형식(테스트 2ch vs 스냅샷)
 *   CONFIG_SMARTGATEWAY_TCP_MODBUS_GATEWAY + sync_gate — 아래 “게이트” 동작
 *
 * 메인 루프(udp_task while(1)):
 *   1) (TCP 게이트 켜짐) TIMESYNC 전이면 sleep 후 continue
 *   2) SMARTGATEWAY_UDP_BIND_PORT>0 이면 udp_try_rx: poll(0) → recvfrom+hex
 *   3) adc_get_latest(&snap) → msgpack → sendto
 *   4) k_msleep(UDP_SEND_INTERVAL_MS)
 *
 * TCP Modbus 게이트웨이가 켜진 빌드:
 *   TCP에서 SYNC(MsgType 0x01)를 받아 sg_timesync_from_tcp_notify()가 호출되기 전
 *   sg_udp_allowed() == false — UDP TX(ADC MessagePack)와 udp_try_rx(RX) 모두 대기.
 *   0x00 수신 후 true: 기존과 같이 주기 송수신.
 *
 * wait_for_network / on_net_event:
 *   __attribute__((unused)) 로 컴파일 유지. 부팅 직후 IP 할당 전에 UDP 시작해야 할 때
 *   호출부에서 연결 가능(이 파일만으로는 자동 호출 안 함).
 */

#include "udp.h"
#include "adc.h"
#include "msgpack_adc.h"
#include "gw_error.h"
#include <stdio.h>
#include <zephyr/autoconf.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_mgmt.h>
#include <string.h>

#if IS_ENABLED(CONFIG_SMARTGATEWAY_TCP_MODBUS_GATEWAY)
#include "sync_gate.h"
#endif

#define UDP_STACK_SIZE	 2048 /* udp_task 스택 크기(바이트) */

/* ── 모드별 UDP 피어·바인드 설정 자동 선택 ─────────────────────────
 *   WIFI_ENABLE=y → WiFi 인터페이스 설정 사용
 *   WIFI_ENABLE=n → 이더넷 인터페이스 설정 사용               */
#if IS_ENABLED(CONFIG_SMARTGATEWAY_WIFI_ENABLE)
#  define UDP_PEER_IP   CONFIG_SMARTGATEWAY_WIFI_UDP_PEER_IP
#  define UDP_PEER_PORT CONFIG_SMARTGATEWAY_WIFI_UDP_PEER_PORT
#  define UDP_BIND_PORT CONFIG_SMARTGATEWAY_WIFI_UDP_BIND_PORT
#else
#  define UDP_PEER_IP   CONFIG_SMARTGATEWAY_ETH_UDP_PEER_IP
#  define UDP_PEER_PORT CONFIG_SMARTGATEWAY_ETH_UDP_PEER_PORT
#  define UDP_BIND_PORT CONFIG_SMARTGATEWAY_ETH_UDP_BIND_PORT
#endif

/* sendto 에 매번 채우는 목적지(초기화 구간에서 한 번 설정) */
static struct sockaddr_in peer_addr;
/* 태스크 전역 UDP fd — 다른 모듈에서 직접 쓰지 말 것(캡슐화 위반 방지) */
static int udp_sock = -1;
/* wait_for_network() 안에서만 의미; 폴링 루프 탈출 플래그 */
static bool net_ready;

/** IPv4 주소가 인터페이스에 붙으면 net_ready = true (콜백) */
static void __attribute__((unused)) on_net_event(struct net_mgmt_event_callback *cb,
						  uint64_t mgmt_event, struct net_if *f)
{
	ARG_UNUSED(cb); /* Zephyr 콜백 시그니처 */
	ARG_UNUSED(f);	/* 관심 이벤트만 사용 */
	if (mgmt_event == NET_EVENT_IPV4_ADDR_ADD) { /* IPv4 주소 할당됨 */
		net_ready = true;
	}
}

/**
 * 기본 인터페이스를 올린 뒤, IPv4 주소가 할당될 때까지 블록.
 * Zephyr net_mgmt 이벤트 콜백으로만 처리(소켓 connect 불필요).
 */
static void __attribute__((unused)) wait_for_network(void)
{
	struct net_mgmt_event_callback mgmt_cb; /* net_mgmt 등록용 콜백 구조체 */

	(void)mgmt_cb;
	printf("[UDP] [CHECK 1] Waiting for network (IPv4 addr)...\n");
	net_ready = false;
	net_mgmt_init_event_callback(&mgmt_cb, on_net_event, NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&mgmt_cb);
	net_if_up(net_if_get_default());

	while (!net_ready) {
		k_msleep(100);
	}
	net_mgmt_del_event_callback(&mgmt_cb);
	printf("[UDP] [CHECK 1] OK - Network ready (IP assigned)\n");
}

/* MessagePack 최대치에 맞춘 TX; RX 스택 버퍼(udp_try_rx 한 번 분량) */
#define UDP_TX_BUF_SIZE  384
#define UDP_RX_BUF_SIZE  512

K_THREAD_STACK_DEFINE(udp_task_stack, UDP_STACK_SIZE); /* udp_task용 스택 영역 */
static struct k_thread udp_task_data;			 /* udp 스레드 제어 블록 */

#if UDP_BIND_PORT > 0
/**
 * bind 된 UDP 소켓에서 읽을 데이터가 있으면 recvfrom 후 앞 64바이트 hex 출력.
 * TCP 게이트웨이 + sync_gate: TIMESYNC(0x00) 전에는 RX 하지 않음.
 */
/* sock: bind 된 UDP 소켓 fd */
static void udp_try_rx(int sock)
{
#if IS_ENABLED(CONFIG_SMARTGATEWAY_TCP_MODBUS_GATEWAY)
	if (!sg_udp_allowed()) {
		return;
	}
#endif

	struct zsock_pollfd pfd = { .fd = sock, .events = ZSOCK_POLLIN };
	/* pfd.revents: poll 이후 실제 이벤트 비트 */

	/* timeout 0 → 논블로킹: 이번 루프 턴에 읽을 것만 확인 */
	if (zsock_poll(&pfd, 1, 0) <= 0 || (pfd.revents & ZSOCK_POLLIN) == 0) {
		return;
	}

	uint8_t rx[UDP_RX_BUF_SIZE]; /* 수신 패킷 한 건 버퍼 */
	struct sockaddr_in from;     /* 송신지 주소(out) */
	socklen_t fromlen = sizeof(from); /* in/out: from 길이 */

	ssize_t r = zsock_recvfrom(sock, rx, sizeof(rx), 0, (struct sockaddr *)&from, &fromlen);
	/* r: 수신 바이트 수 */

	if (r < 0) {
		printf("[UDP] recvfrom err\n");
		return;
	}
	if (r == 0) {
		return;
	}

	printf("[UDP] RX %zd B from %d.%d.%d.%d:%u:", r, (int)((uint8_t *)&from.sin_addr)[0],
	       (int)((uint8_t *)&from.sin_addr)[1], (int)((uint8_t *)&from.sin_addr)[2],
	       (int)((uint8_t *)&from.sin_addr)[3], (unsigned)ntohs(from.sin_port));
	/* 콘솔 과부하 방지: 앞 64바이트만 덤프 */
	for (ssize_t i = 0; i < r && i < 64; i++) { /* i: hex 출력용 인덱스 */
		printf(" %02x", rx[i]);
	}
	if (r > 64) {
		printf(" ...");
	}
	printf("\n");
}
#endif

/**
 * UDP 소켓 생성 → (옵션) bind → 피어 주소 설정 후 무한 루프:
 * 선택 RX, ADC 읽기, MessagePack 인코딩, sendto, INTERVAL 대기.
 */
static void udp_task(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); /* k_thread_create 인자(미사용) */
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	udp_sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (udp_sock < 0) {
		printf("[UDP] [CHECK 2] FAIL - Socket create error: %d\n", udp_sock);
		gw_error_set(GW_ERR_UDP_COMM);
		return;
	}
	printf("[UDP] [CHECK 2] OK - Socket created (fd=%d)\n", udp_sock);

#if UDP_BIND_PORT > 0
	struct sockaddr_in bind_sa = { 0 }; /* 로컬 수신 주소 */

	bind_sa.sin_family = AF_INET;
	bind_sa.sin_port = htons((uint16_t)UDP_BIND_PORT);
	bind_sa.sin_addr.s_addr = htonl(INADDR_ANY); /* 모든 인터페이스 */
	if (zsock_bind(udp_sock, (struct sockaddr *)&bind_sa, sizeof(bind_sa)) != 0) {
		printf("[UDP] bind port %u FAIL\n", (unsigned)UDP_BIND_PORT);
		gw_error_set(GW_ERR_UDP_COMM);
		zsock_close(udp_sock);
		return;
	}
#if IS_ENABLED(CONFIG_SMARTGATEWAY_TCP_MODBUS_GATEWAY)
	printf("[UDP] bind :%u (TX/RX after TCP 0x00 TIMESYNC)\n",
	       (unsigned)UDP_BIND_PORT);
#else
	printf("[UDP] bind :%u (RX enabled)\n", (unsigned)UDP_BIND_PORT);
#endif
#endif

	memset(&peer_addr, 0, sizeof(peer_addr));
	peer_addr.sin_family = AF_INET;
	peer_addr.sin_port = htons((uint16_t)UDP_PEER_PORT);
	/* pton 실패 시 sin_addr=0 이 될 수 있음 — 운영에서는 PEER_IP 검증 권장 */
	zsock_inet_pton(AF_INET, UDP_PEER_IP, &peer_addr.sin_addr);
	printf("[UDP] TX target: %s:%d, interval %dms, test_mode=%d\n",
	       UDP_PEER_IP, UDP_PEER_PORT,
	       UDP_SEND_INTERVAL_MS, IS_ENABLED(CONFIG_SMARTGATEWAY_UDP_TEST_MODE));

	uint8_t tx_buf[UDP_TX_BUF_SIZE]; /* MessagePack 출력 버퍼 */
	/* ADC 샘플 미준비 연속일 때 로그만 살짝 */
	static uint32_t skip_count; /* 연속 skip 횟수(로그 스팸 제한) */

	while (1) {
#if IS_ENABLED(CONFIG_SMARTGATEWAY_TCP_MODBUS_GATEWAY)
		if (!sg_udp_allowed()) {
			static bool udp_gate_msg;

			if (!udp_gate_msg) {
				printf("[UDP] waiting for TCP SYNC (0x01) — no TX/RX yet\n");
				udp_gate_msg = true;
			}
			k_msleep(UDP_SEND_INTERVAL_MS);
			continue;
		}
#endif

#if UDP_BIND_PORT > 0
		udp_try_rx(udp_sock);
#endif

		adc_snapshot_t snap; /* ADC 채널 스냅샷(인코딩 원천) */
		if (adc_get_latest(&snap) != 0) {
			skip_count++;
			if (skip_count <= 5 || skip_count % 10 == 0) {
				printf("[UDP] [CHECK 4] No ADC data yet (skip #%u)\n", skip_count);
			}
			k_msleep(UDP_SEND_INTERVAL_MS);
			continue;
		}
		skip_count = 0;

		int len; /* 인코딩된 MessagePack 바이트 수 */
		if (IS_ENABLED(CONFIG_SMARTGATEWAY_UDP_TEST_MODE)) {
			len = msgpack_encode_adc_test_2ch(&snap, tx_buf, sizeof(tx_buf));
		} else {
			len = msgpack_encode_adc_snapshot(&snap, tx_buf, sizeof(tx_buf));
		}
		if (len <= 0) {
			printf("[UDP] [CHECK 5] FAIL - MessagePack encode error: %d\n", len);
			k_msleep(UDP_SEND_INTERVAL_MS);
			continue;
		}

		ssize_t sent = zsock_sendto(udp_sock, tx_buf, len, 0,
					    (struct sockaddr *)&peer_addr,
					    sizeof(peer_addr));
		/* sent: 실제로 나간 UDP 페이로드 바이트 수(sendto는 블로킹 계열) */
		if (sent != len) {
			printf("[UDP] [CHECK 6] FAIL - Send error: %zd (expected %d)\n", sent, len);
			gw_error_set(GW_ERR_UDP_COMM);
		} else if (gw_error_get() == GW_ERR_UDP_COMM) {
			gw_error_clear();
		}
		k_msleep(UDP_SEND_INTERVAL_MS);
	}
}

/**
 * 우선순위 4: ADC(3) 다음, tcp_gateway(6)보다 높게 — Modbus/TCP가 UDP보다 우선.
 */
int udp_task_start(void)
{
	k_tid_t tid = k_thread_create(&udp_task_data, udp_task_stack,
				      K_THREAD_STACK_SIZEOF(udp_task_stack),
				      udp_task, NULL, NULL, NULL, 4, 0, K_NO_WAIT);
	/* tid: udp_task 스레드 핸들; NULL이면 생성 실패 */

	if (tid == NULL) {
		return -1;
	}
	k_thread_name_set(tid, "udp_task");
	return 0;
}
