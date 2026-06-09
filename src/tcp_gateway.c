/*
 * Smart Gateway — TCP 클라이언트 ↔ RS-232 Modbus 게이트웨이
 *
 * 보드가 원격 서버에 connect. 탈취·listen 서버 모드 없음.
 * 연결 후 HANDSHAKE_POLL_MS 간격 0x80(CONNECT) → 서버 0x01(시각+RS-232) → 보드 0x81 ACK → 세션 0x02/0x82.
 */

#include "tcp_gateway.h"
#include "rs232.h"
#include "sync_gate.h"
#include "config_nvs.h"
#include "network_manager.h"
#include "time_helper.h"
#include "gw_error.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/autoconf.h>
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

/* --- 외곽 프레임 / MsgType 상수 --- */

#define GW_STX			  			0x55	/* 프레임 시작 바이트 */
#define GW_ETX			  			0x03	/* 프레임 종료 바이트 */
/* ② 핸드셰이크 서버→보드: STX + MsgType + Seq + Body + ETX */
#define GW_SRV_RX_HDR3_LEN	  		3U		/* STX(1)+MsgType(1)+Seq(1) */
#define GW_SRV_RX_TAIL_LEN	  		1U		/* ETX(1) */
/* ③ 세션 서버→보드: STX + Len(2,BE) + MsgType + Seq + Body + ETX */
#define GW_SRV_SESS_RX_HDR_LEN	  	5U		/* STX(1)+Len(2)+MsgType(1)+Seq(1) */
#define GW_SRV_SESS_RX_TAIL_LEN	  	1U		/* ETX(1) */
#define GW_TIME_WIRE_LEN			9U		/* yyyy(2)+MM+dd+HH+mm+ss+msec(2) */
#define GW_RS232_CFG_WIRE_LEN		5U		/* BPS, DataBit, StopBit, Parity, Flow */
#define GW_DEVICE_INDEX_WIRE_LEN	1U
#define GW_VERSION_WIRE_LEN			3U
#define GW_SYNC_PAYLOAD_LEN		(GW_TIME_WIRE_LEN + GW_RS232_CFG_WIRE_LEN)
#define GW_RS232_CFG_BODY_OFFSET	GW_TIME_WIRE_LEN
#define GW_SYNC_COMPACT_LEN		(GW_SRV_RX_HDR3_LEN + GW_SYNC_PAYLOAD_LEN + GW_SRV_RX_TAIL_LEN)
#define GW_MASTER_CODE_MAX_LEN		32U
/* 보드→서버: STX + Len_BE + MsgType + Seq + Body + ETX */
#define GW_TX_HDR_LEN		  		5U		/* STX(1)+Len(2)+MsgType(1)+Seq(1) */
#define GW_TX_TAIL_LEN		  		1U		/* ETX(1) */
// SmartGateway -> Server
#define GW_MSG_CONNECT		  		0x80	/* 보드→서버: 장치코드+IDX+버전 */
#define GW_MSG_ACK					0x81	/* 보드→서버: RS-232 설정 결과 Ret */
#define GW_MSG_MODBUS_RESP			0x82	/* 보드→서버: 시각+Modbus 응답 (Len 프레임) */
// SmartGateway <- Server
#define GW_MSG_SYNC					0x01	/* 서버→보드: 시각+RS-232 (compact) */
#define GW_MSG_MODBUS_REQ			0x02	/* 서버→보드: Modbus RTU 요청 (Len 프레임) */
#define GW_BODY_CAP		  			256		/* 정적 버퍼(gw_mb_resp 등) 최대 Body 바이트 */
#define GW_RESPONSE_BODY_CAP		(GW_TIME_WIRE_LEN + GW_BODY_CAP)
#define GW_RS232_DATA_PORT			1U

BUILD_ASSERT(CONFIG_SMARTGATEWAY_TCP_MAX_BODY <= GW_BODY_CAP);
#define GW_MAX_BODY CONFIG_SMARTGATEWAY_TCP_MAX_BODY /* Kconfig: 프레임 Body 최대 허용 */

BUILD_ASSERT(CONFIG_SMARTGATEWAY_TCP_STREAM_BUF >=
	     (GW_SRV_SESS_RX_HDR_LEN + GW_MAX_BODY + GW_SRV_SESS_RX_TAIL_LEN));
BUILD_ASSERT(GW_SYNC_PAYLOAD_LEN <= GW_BODY_CAP);

#define TCP_GW_STACK 2048
K_THREAD_STACK_DEFINE(tcp_gw_stack, TCP_GW_STACK);
static struct k_thread tcp_gw_thr;

/* TCP 수신 누적 버퍼: 한 번에 오지 않는 프레임을 맞추기 위해 append 후 파싱 */
static uint8_t gw_rx_buf[CONFIG_SMARTGATEWAY_TCP_STREAM_BUF];
/* RS-232(Modbus RTU) 슬레이브 응답 임시 보관 → 0x82 Body로 재포장 */
static uint8_t gw_mb_resp[GW_BODY_CAP];
/* 송신: ① compact(3+N+1) 또는 ④ 긴 프레임(5+N+2) — 버퍼는 ④ 기준 */
static uint8_t gw_tx_frame[GW_TX_HDR_LEN + GW_RESPONSE_BODY_CAP + GW_TX_TAIL_LEN];

/** 0x80(CONNECT) Body: gw_init_connect_body_once() */
static uint8_t gw_connect_body[GW_BODY_CAP];
static uint16_t gw_connect_body_len;

static int gw_hex_digit(int c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return 10 + (c - 'a');
	}
	if (c >= 'A' && c <= 'F') {
		return 10 + (c - 'A');
	}
	return -1;
}

/** src: 연속 16진 문자열(공백 무시). 성공 시 바이트 수, 실패 시 -1 */
static int gw_parse_hex_into(const char *src, uint8_t *dst, size_t dst_cap)
{
	size_t n = 0;
	size_t i = 0;

	while (src[i] != '\0') {
		while (src[i] == ' ' || src[i] == '\t') {
			i++;
		}
		if (src[i] == '\0') {
			break;
		}
		int hi = gw_hex_digit((unsigned char)src[i++]);
		if (hi < 0) {
			return -1;
		}
		while (src[i] == ' ' || src[i] == '\t') {
			i++;
		}
		if (src[i] == '\0') {
			return -1;
		}
		int lo = gw_hex_digit((unsigned char)src[i++]);
		if (lo < 0) {
			return -1;
		}
		if (n >= dst_cap) {
			return -1;
		}
		dst[n++] = (uint8_t)(((unsigned)hi << 4) | (unsigned)lo);
	}
	return (int)n;
}

static void gw_init_connect_body_once(void)
{
	static bool done;
	char ver_hex[48];
	size_t master_len;
	size_t ver_off;
	const size_t tail_len = GW_DEVICE_INDEX_WIRE_LEN + GW_VERSION_WIRE_LEN;

	if (done) {
		return;
	}
	done = true;

	master_len = strnlen(g_gw_config.master_code, GW_MASTER_CODE_MAX_LEN);
	if (master_len + tail_len > GW_BODY_CAP) {
		master_len = GW_BODY_CAP - tail_len;
		printf("[GW] CONNECT master truncated to %u bytes\n", (unsigned)master_len);
	}

	memset(gw_connect_body, 0, master_len + tail_len);
	memcpy(gw_connect_body, g_gw_config.master_code, master_len);
	gw_connect_body[master_len] = (uint8_t)(g_gw_config.device_index & 0xffU);

	ver_off = master_len + GW_DEVICE_INDEX_WIRE_LEN;
	strncpy(ver_hex, CONFIG_SMARTGATEWAY_TCP_CONNECT_VERSION_HEX, sizeof(ver_hex) - 1);
	ver_hex[sizeof(ver_hex) - 1] = '\0';

	if (gw_parse_hex_into(ver_hex, gw_connect_body + ver_off, GW_VERSION_WIRE_LEN) !=
	    (int)GW_VERSION_WIRE_LEN) {
		printf("[GW] CONNECT: invalid SMARTGATEWAY_TCP_CONNECT_VERSION_HEX, use 2.0.0\n");
		gw_connect_body[ver_off + 0U] = 0x02;
		gw_connect_body[ver_off + 1U] = 0x00;
		gw_connect_body[ver_off + 2U] = 0x00;
	}

	gw_connect_body_len = (uint16_t)(master_len + tail_len);
}

/** 디버그: 방향 태그와 함께 hex 덤프 */
/* dir_tag: 로그 방향 레이블. p: 덤프 시작. n: 덤프 바이트 수 */
static void gw_hex_line(const char *dir_tag, const uint8_t *p, size_t n)
{
	printf("[GW] %s %u B:", dir_tag, (unsigned)n);
	for (size_t i = 0; i < n; i++) { /* i: 출력 중인 바이트 인덱스 */
		printf(" %02x", p[i]);
	}
	printf("\n");
}

/**
 * 서버→보드 프레임 길이 계산. p[0]이 STX라고 가정(호출 전 동기화).
 * 반환 1: *need_out·*bl_out 유효, rlen >= *need_out 이면 완전 프레임.
 * 반환 0: 아직 바이트 부족.
 * 반환 -1: 동기 오류(STX 아님·ETX 불일치·Body 과대 등) → 보통 STX 1바이트 스킵.
 */
static int gw_srv_rx_frame_info(const uint8_t *p, size_t rlen, size_t *need_out, uint16_t *bl_out,
				int *why_out)
{
	*why_out = 0;
	if (rlen < GW_SRV_RX_HDR3_LEN) {
		return 0;
	}
	if (p[0] != GW_STX) {
		*why_out = 2;
		return -1;
	}

	if (p[1] == GW_MSG_SYNC) {
		/* 0x01: Body 고정 14B(시각 9 + RS-232 5) → 프레임 18B */
		*bl_out = (uint16_t)GW_SYNC_PAYLOAD_LEN;
		*need_out = GW_SYNC_COMPACT_LEN;
		if (rlen < *need_out) {
			return 0;
		}
		if (p[*need_out - 1U] != GW_ETX) {
			*why_out = 5;
			return -1;
		}
		return 1;
	}

	for (size_t i = GW_SRV_RX_HDR3_LEN; i < rlen; i++) {
		if (p[i] == GW_ETX) {
			*bl_out = (uint16_t)(i - GW_SRV_RX_HDR3_LEN);
			*need_out = i + 1U;
			if (*bl_out > GW_MAX_BODY) {
				*why_out = 3;
				return -1;
			}
			return 1;
		}
		if ((i - GW_SRV_RX_HDR3_LEN) >= (size_t)GW_MAX_BODY) {
			*why_out = 3;
			return -1;
		}
	}
	return 0;
}

/**
 * ③ 세션 서버→보드: STX + Len_BE + MsgType + Seq + Body + ETX (Error 없음).
 */
static int gw_srv_sess_rx_frame_info(const uint8_t *p, size_t rlen, size_t *need_out, uint16_t *bl_out,
				     int *why_out)
{
	*why_out = 0;
	if (rlen < GW_SRV_SESS_RX_HDR_LEN + GW_SRV_SESS_RX_TAIL_LEN) {
		return 0;
	}
	if (p[0] != GW_STX) {
		*why_out = 2;
		return -1;
	}

	uint16_t body_len = sys_get_be16(p + 1);

	if (body_len > GW_MAX_BODY) {
		*why_out = 3;
		return -1;
	}

	*bl_out = body_len;
	*need_out = (size_t)GW_SRV_SESS_RX_HDR_LEN + body_len + GW_SRV_SESS_RX_TAIL_LEN;
	if (rlen < *need_out) {
		return 0;
	}
	if (p[(size_t)GW_SRV_SESS_RX_HDR_LEN + body_len] != GW_ETX) {
		*why_out = 5;
		return -1;
	}
	return 1;
}

/** ① 보드→서버(연결 통지만): STX + MsgType + Seq + Body + ETX */
static int gw_build_compact_tx(uint8_t *out, size_t out_cap, uint8_t msg_type, uint8_t seq,
			       const uint8_t *body, uint16_t body_len)
{
	const size_t need = GW_SRV_RX_HDR3_LEN + body_len + GW_SRV_RX_TAIL_LEN;

	if (out_cap < need || body_len > GW_MAX_BODY) {
		return -EINVAL;
	}
	if (body_len > 0U && body == NULL) {
		return -EINVAL;
	}
	out[0] = GW_STX;
	out[1] = msg_type;
	out[2] = seq;
	if (body_len > 0U) {
		memcpy(out + GW_SRV_RX_HDR3_LEN, body, body_len);
	}
	out[GW_SRV_RX_HDR3_LEN + body_len] = GW_ETX;
	return (int)need;
}

/**
 * UART에서 받은 Modbus RTU 응답을 사람이 읽기 쉬운 형태로 로그.
 * - mb[0]: 슬레이브 주소, mb[1]: 기능코드(상위비트 set 이면 예외 응답)
 * - FC03(0x03) 읽기 + 바이트수 6 + 길이 충분하면 D7002~D7004(raw/10) 가정해 출력(장비 맞춤)
 * - 그 외는 전체 hex
 */
static void gw_print_modbus_fc03_like(const uint8_t *mb, size_t mb_len)
{
	if (mb_len < 5) {
		gw_hex_line("Modbus RX(short)", mb, mb_len);
		return;
	}
	/* 예: 요청 FC=0x03 → 예외 시 응답 FC=0x83 */
	if (mb[1] == (0x03 | 0x80)) {
		printf("[GW] Modbus except FC=0x%02x code=0x%02x\n", mb[1], mb[2]);
		gw_hex_line("Modbus except", mb, mb_len);
		return;
	}
	if (mb[1] == 0x03 && mb[2] == 6 && mb_len >= 11) {
		uint16_t r0 = sys_get_be16(mb + 3); /* 레지스터 영역 raw 값 1 (스케일 /10 가정) */
		uint16_t r1 = sys_get_be16(mb + 5); /* raw 값 2 */
		uint16_t r2 = sys_get_be16(mb + 7); /* raw 값 3 */

		printf("[GW] FC03 D7002=%.1f D7003=%.1f D7004=%.1f (raw %u %u %u)\n",
		       (double)r0 / 10.0, (double)r1 / 10.0, (double)r2 / 10.0, r0, r1, r2);
	} else {
		gw_hex_line("Modbus RX", mb, mb_len);
	}
}

/** TCP 소켓에 n바이트 모두 보낼 때까지 반복(부분 송신 처리) */
static int gw_send_all(int fd, const uint8_t *p, size_t n, const char *tag)
{
	/* fd: 소켓. p: 송신 버퍼. n: 총 길이. tag: 디버그용(현재 미사용) */
	size_t sent = 0; /* 지금까지 전송 완료한 바이트 수 */

	ARG_UNUSED(tag);

	while (sent < n) {
		ssize_t r = zsock_send(fd, p + sent, n - sent, 0); /* 이번 send에서 나간 바이트 수 */

		if (r < 0) {
			printf("[GW] TCP-> send fail pos=%zu errno=%d\n", sent, errno);
			return -1;
		}
		if (r == 0) {
			printf("[GW] TCP-> send 0 B (disconnect) pos=%zu\n", sent);
			return -1;
		}
		sent += (size_t)r;
	}
	return 0;
}

int gw_tcp_send_alarm(int cfd, uint8_t seq, uint8_t code)
{
	uint8_t body = code;
	int plen;

	if (code == GW_ERR_NONE) {
		return 0;
	}

	plen = gw_build_compact_tx(gw_tx_frame, sizeof(gw_tx_frame), GW_MSG_ALARM, seq, &body, 1);
	if (plen <= 0) {
		printf("[GW] 0x8E alarm build fail code=0x%02x\n", code);
		return -1;
	}
	gw_hex_line("SmartGateway->Server (0x8E)", gw_tx_frame, (size_t)plen);
	if (gw_send_all(cfd, gw_tx_frame, (size_t)plen, "alarm") != 0) {
		printf("[GW] 0x8E alarm send fail code=0x%02x\n", code);
		return -1;
	}
	return 0;
}

static void gw_report_error(int cfd, uint8_t seq, uint8_t code)
{
	gw_error_set(code);
	(void)gw_tcp_send_alarm(cfd, seq, code);
}

/**
 * 0x01 SYNC Body: 시간(9) + RS-232(5) = 14B.
 */
static void gw_apply_sync_and_rs232(int cfd, const uint8_t *body, size_t body_len, uint8_t seq)
{
	uint8_t resp_byte = 0x01;
	int plen;
	uint8_t chk;

	if (body == NULL || body_len != GW_SYNC_PAYLOAD_LEN) {
		printf("[GW] 0x01 SYNC payload bad len (body_len=%u, need %u)\n",
		       (unsigned)body_len, (unsigned)GW_SYNC_PAYLOAD_LEN);
		gw_report_error(cfd, seq, GW_ERR_INIT_CFG);
		goto send_ack;
	}

	chk = gw_sync_body_check(body, body_len);
	if (chk != GW_ERR_NONE) {
		printf("[GW] 0x01 SYNC invalid time/RS-232 cfg\n");
		gw_report_error(cfd, seq, chk);
		goto send_ack;
	}

	sg_timesync_from_tcp_notify(body, GW_TIME_WIRE_LEN);

	rs232_remote_cfg_t cfg = {
		.protocol = 0x01,
		.bps      = body[GW_RS232_CFG_BODY_OFFSET + 0],
		.data_bit = body[GW_RS232_CFG_BODY_OFFSET + 1],
		.stop_bit = body[GW_RS232_CFG_BODY_OFFSET + 2],
		.parity   = body[GW_RS232_CFG_BODY_OFFSET + 3],
		.flow     = body[GW_RS232_CFG_BODY_OFFSET + 4],
	};

	int ret = rs232_reconfigure(GW_RS232_DATA_PORT, &cfg);

	if (ret != 0) {
		printf("[GW] UART%u RS-232 reconfigure NG\n", (unsigned)GW_RS232_DATA_PORT);
		gw_report_error(cfd, seq, GW_ERR_INIT_CFG);
		goto send_ack;
	}

	gw_error_clear();
	resp_byte = 0x00;

	printf("[GW] UART%u RS-232 reconfigure OK — sending 0x81 ACK\n",
	       (unsigned)GW_RS232_DATA_PORT);

send_ack:
	plen = gw_build_compact_tx(gw_tx_frame, sizeof(gw_tx_frame),
				   GW_MSG_ACK, seq, &resp_byte, 1);

	if (plen > 0) {
		gw_hex_line("SmartGateway->Server", gw_tx_frame, (size_t)plen);
		if (gw_send_all(cfd, gw_tx_frame, (size_t)plen, "rs232cfg") != 0) {
			printf("[GW] 0x81 RS232-CFG response send failed\n");
		}
	}
}

static int gw_build_response82_tx(uint8_t seq, const uint8_t *rs_resp, uint16_t rs_len)
{
	if (rs_len > GW_BODY_CAP || (rs_len > 0U && rs_resp == NULL)) {
		return -EINVAL;
	}

	uint16_t body_len = (uint16_t)(GW_TIME_WIRE_LEN + rs_len);
	size_t need = GW_TX_HDR_LEN + body_len + GW_TX_TAIL_LEN;

	if (sizeof(gw_tx_frame) < need) {
		return -ENOMEM;
	}

	gw_tx_frame[0] = GW_STX;
	sys_put_be16(body_len, gw_tx_frame + 1);
	gw_tx_frame[3] = GW_MSG_MODBUS_RESP;
	gw_tx_frame[4] = seq;
	time_encode_now_9(gw_tx_frame + GW_TX_HDR_LEN);
	if (rs_len > 0U) {
		memcpy(gw_tx_frame + GW_TX_HDR_LEN + GW_TIME_WIRE_LEN, rs_resp, rs_len);
	}
	gw_tx_frame[GW_TX_HDR_LEN + body_len] = GW_ETX;
	return (int)need;
}

static bool gw_process_frames_handshake(int cfd, uint8_t *rbuf, size_t *rlen)
{
	bool got_sync = false;

	while (*rlen >= GW_SRV_RX_HDR3_LEN) {
		size_t i = 0;

		while (i < *rlen && rbuf[i] != GW_STX) {
			i++;
		}
		if (i > 0) {
			memmove(rbuf, rbuf + i, *rlen - i);
			*rlen -= i;
		}
		if (*rlen < GW_SRV_RX_HDR3_LEN) {
			break;
		}

		size_t need = 0;
		uint16_t bl = 0;
		int why = 0;
		int st = gw_srv_rx_frame_info(rbuf, *rlen, &need, &bl, &why);

		if (st < 0) {
			if (why != 0) {
				printf("[GW] handshake srv-rx bad (why=%d) — skip STX\n", why);
			}
			memmove(rbuf, rbuf + 1, *rlen - 1);
			(*rlen)--;
			continue;
		}
		if (st == 0 || *rlen < need) {
			break;
		}

		uint8_t msg_type = rbuf[1];
		uint8_t seq = rbuf[2];
		const uint8_t *body = (bl > 0U) ? (rbuf + GW_SRV_RX_HDR3_LEN) : NULL;

		gw_hex_line("SmartGateway<-Server", rbuf, need);
		if (msg_type == GW_MSG_SYNC) {
			gw_apply_sync_and_rs232(cfd, body, bl, seq);
			got_sync = true;
		} else {
			printf("[GW] handshake skip MsgType 0x%02x (wait 0x01)\n", msg_type);
		}

		memmove(rbuf, rbuf + need, *rlen - need);
		*rlen -= need;
	}
	return got_sync;
}

/**
 * 핸드셰이크: 서버 0x01 SYNC 올 때까지 HANDSHAKE_POLL_MS 마다 0x80(CONNECT) 재전송.
 */
static bool gw_tcp_handshake(int cfd, uint8_t *rbuf, size_t *rlen)
{
	const int period_ms = (int)CONFIG_SMARTGATEWAY_TCP_HANDSHAKE_POLL_MS;

	gw_init_connect_body_once();

	/* next_send_ms == 0 → 첫 프레임은 즉시 송신 */
	int64_t next_send_ms = 0;

	for (;;) {
		int64_t now = k_uptime_get();

		if (now >= next_send_ms) {
			int plen = gw_build_compact_tx(gw_tx_frame, sizeof(gw_tx_frame), GW_MSG_CONNECT, 0,
						       gw_connect_body, gw_connect_body_len);

			if (plen > 0) {
				if (gw_send_all(cfd, gw_tx_frame, (size_t)plen, "ann") != 0) {
					printf("[GW] 0x80 CONNECT send failed\n");
					gw_error_set(GW_ERR_TCP_COMM);
					return false;
				}
				gw_hex_line("SmartGateway->Server", gw_tx_frame, (size_t)plen);
			}
			next_send_ms = now + (int64_t)period_ms;
		}

		int64_t ms_left = next_send_ms - k_uptime_get();
		int poll_to = 0;

		if (ms_left > 0) {
			poll_to = (ms_left > (int64_t)period_ms) ? period_ms : (int)ms_left;
			if (poll_to < 1) {
				poll_to = 1;
			}
		}

		struct zsock_pollfd pfd = { .fd = cfd, .events = ZSOCK_POLLIN };
		int pr = zsock_poll(&pfd, 1, poll_to);

		if (pr < 0) {
			printf("[GW] handshake poll errno=%d\n", errno);
			gw_error_set(GW_ERR_TCP_COMM);
			return false;
		}
		if (pr > 0) {
			if (pfd.revents & (ZSOCK_POLLHUP | ZSOCK_POLLERR)) {
				printf("[GW] handshake peer closed\n");
				gw_error_set(GW_ERR_TCP_COMM);
				return false;
			}
			if (pfd.revents & ZSOCK_POLLIN) {
				if (*rlen >= sizeof(gw_rx_buf)) {
					*rlen = 0;
				}
				ssize_t n = zsock_recv(cfd, rbuf + *rlen, sizeof(gw_rx_buf) - *rlen, 0);

				if (n == 0) {
					printf("[GW] handshake peer closed\n");
					gw_error_set(GW_ERR_TCP_COMM);
					return false;
				}
				if (n < 0) {
					printf("[GW] handshake recv errno=%d\n", errno);
					gw_error_set(GW_ERR_TCP_COMM);
					return false;
				}
				*rlen += (size_t)n;
				if (gw_process_frames_handshake(cfd, rbuf, rlen)) {
					return true;
				}
			}
		}
	}
}

/**
 * 단일 TCP 클라이언트 세션.
 * 1) gw_tcp_handshake (0x80↔0x01 SYNC + 0x81 ACK)
 * 2) 세션: STX+Len+MsgType+Seq+Body+ETX — 서버 0x02 Modbus 요청, 보드 0x82 응답.
 *
 * bl > MAX_BODY 이면 rlen=0 리셋.
 * 파싱 실패(reason 로그)는 바이트 하나만 밀어서 재동기(앞선 STX가 노이즈였을 수 있음).
 */
static void gw_handle_client(int cfd)
{
	uint8_t *const rbuf = gw_rx_buf;
	size_t rlen = 0;

	if (!gw_tcp_handshake(cfd, rbuf, &rlen)) {
		printf("[GW] handshake failed\n");
		gw_error_set(GW_ERR_TCP_COMM);
		return;
	}

	gw_error_clear();

	for (;;) {
		if (rlen >= sizeof(gw_rx_buf)) {
			printf("[GW] TCP rx buffer full — reset\n");
			rlen = 0;
		}

		/* 블로킹 recv 대신 300ms poll → 플래그 재확인 (Zephyr shutdown 미지원 대비) */
		struct zsock_pollfd mpfd = { .fd = cfd, .events = ZSOCK_POLLIN };
		int pr = zsock_poll(&mpfd, 1, 300);

		if (pr < 0) {
			printf("[GW] recv poll errno=%d\n", errno);
			gw_error_set(GW_ERR_TCP_COMM);
			break;
		}
		if (pr == 0) {
			continue; /* 300ms timeout — 플래그 재확인 */
		}
		if (mpfd.revents & (ZSOCK_POLLHUP | ZSOCK_POLLERR)) {
			printf("[GW] peer closed\n");
			gw_error_set(GW_ERR_TCP_COMM);
			break;
		}
		if (!(mpfd.revents & ZSOCK_POLLIN)) {
			continue;
		}

		ssize_t n = zsock_recv(cfd, rbuf + rlen, sizeof(gw_rx_buf) - rlen, 0);
		if (n == 0) {
			printf("[GW] peer closed\n");
			gw_error_set(GW_ERR_TCP_COMM);
			break;
		}
		if (n < 0) {
			printf("[GW] recv errno=%d\n", errno);
			gw_error_set(GW_ERR_TCP_COMM);
			break;
		}

		rlen += (size_t)n;
		while (rlen >= GW_SRV_SESS_RX_HDR_LEN + GW_SRV_SESS_RX_TAIL_LEN) {
			size_t i = 0;

			while (i < rlen && rbuf[i] != GW_STX) {
				i++;
			}
			if (i > 0) {
				memmove(rbuf, rbuf + i, rlen - i);
				rlen -= i;
			}

			if (rlen < GW_SRV_SESS_RX_HDR_LEN + GW_SRV_SESS_RX_TAIL_LEN) {
				break;
			}

			size_t need = 0;
			uint16_t bl = 0;
			int why = 0;
			int st = gw_srv_sess_rx_frame_info(rbuf, rlen, &need, &bl, &why);

			if (st < 0) {
				printf("[GW] srv->GW sess frame bad why=%d -- skip STX\n", why);
				memmove(rbuf, rbuf + 1, rlen - 1);
				rlen--;
				continue;
			}
			if (st == 0 || rlen < need) {
				break;
			}

			uint8_t msg_type = rbuf[3];
			uint8_t seq = rbuf[4];
			const uint8_t *body = (bl > 0U) ? (rbuf + GW_SRV_SESS_RX_HDR_LEN) : NULL;

			if (msg_type == GW_MSG_SYNC) {
				gw_hex_line("SmartGateway<-Server (0x01 SYNC in session)", rbuf, need);
				gw_apply_sync_and_rs232(cfd, body, bl, seq);
				memmove(rbuf, rbuf + need, rlen - need);
				rlen -= need;
				continue;
			}
			if (msg_type != GW_MSG_MODBUS_REQ) {
				printf("[GW] skip MsgType 0x%02x (not 0x02)\n", msg_type);
				memmove(rbuf, rbuf + need, rlen - need);
				rlen -= need;
				continue;
			}
			if (body == NULL || bl == 0U) {
				printf("[GW] request with empty Body — skip\n");
				memmove(rbuf, rbuf + need, rlen - need);
				rlen -= need;
				continue;
			}

			gw_hex_line("SmartGateway<-Server", rbuf, need);

			size_t mb_len = 0;
			int mbr = -ETIMEDOUT;
			int attempt;

			for (attempt = 0; attempt < GW_RS232_MODBUS_RETRIES; attempt++) {
				mb_len = 0;
				mbr = rs232_modbus_txrx(GW_RS232_DATA_PORT, body, bl,
							gw_mb_resp, sizeof(gw_mb_resp), &mb_len);
				if (mbr == 0 && mb_len > 0U) {
					if (gw_error_get() == GW_ERR_RS232_NO_RESPONSE) {
						gw_error_clear();
					}
					break;
				}
				if (attempt + 1 < GW_RS232_MODBUS_RETRIES) {
					printf("[GW] RS-232 no response (try %d/%d) ret=%d\n",
					       attempt + 1, GW_RS232_MODBUS_RETRIES, mbr);
				}
			}

			if (mbr != 0 || mb_len == 0U) {
				printf("[GW] UART%u RS-232 fail after %d tries ret=%d rx_len=%u"
				       " — 0x8E only (no 0x82)\n",
				       (unsigned)GW_RS232_DATA_PORT, GW_RS232_MODBUS_RETRIES, mbr,
				       (unsigned)mb_len);
				gw_report_error(cfd, seq, GW_ERR_RS232_NO_RESPONSE);
			} else {
				gw_print_modbus_fc03_like(gw_mb_resp, mb_len);
				int olen = gw_build_response82_tx(seq, gw_mb_resp, (uint16_t)mb_len);

				if (olen > 0) {
					gw_hex_line("SmartGateway->Server", gw_tx_frame, (size_t)olen);
					if (gw_send_all(cfd, gw_tx_frame, (size_t)olen, "ok") != 0) {
						printf("[GW] SmartGateway->Server ok response send fail\n");
					}
				} else {
					printf("[GW] gw_build_response82_tx(ok) fail olen=%d mb_len=%u cap=%zu\n",
					       olen, (unsigned)mb_len, sizeof(gw_tx_frame));
				}
			}
			k_msleep(10);

			memmove(rbuf, rbuf + need, rlen - need);
			rlen -= need;
		}
	}
}

static void tcp_gateway_task(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

#if CONFIG_SMARTGATEWAY_TCP_START_DELAY_MS > 0
	printf("[GW] TCP client start delay %u ms\n",
	       (unsigned)CONFIG_SMARTGATEWAY_TCP_START_DELAY_MS);
	k_msleep(CONFIG_SMARTGATEWAY_TCP_START_DELAY_MS);
#endif

	for (;;) {
		const char *peer_host = netmgr_tcp_peer_ip();
		uint16_t    peer_port = netmgr_tcp_peer_port();

		if (peer_host == NULL || peer_host[0] == '\0' || peer_port == 0U) {
			peer_host = CONFIG_SMARTGATEWAY_TCP_PEER_IP;
			peer_port = (uint16_t)CONFIG_SMARTGATEWAY_TCP_PEER_PORT;
		}

#if IS_ENABLED(CONFIG_SMARTGATEWAY_DNS_TEST_MODE)
		peer_host = g_gw_config.server_domain;
		peer_port = g_gw_config.server_domain_port;
#endif

		int cfd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

		if (cfd < 0) {
			printf("[GW] socket() failed\n");
			k_msleep(CONFIG_SMARTGATEWAY_TCP_RECONNECT_MS);
			continue;
		}

#if IS_ENABLED(CONFIG_SMARTGATEWAY_DNS_TEST_MODE)
		{
			char port_str[8];
			struct zsock_addrinfo hints = {
				.ai_family   = AF_INET,
				.ai_socktype = SOCK_STREAM,
			};
			struct zsock_addrinfo *res = NULL;

			snprintf(port_str, sizeof(port_str), "%u", (unsigned)peer_port);
			printf("[GW] DNS resolve '%s'...\n", peer_host);
			int gaierr = zsock_getaddrinfo(peer_host, port_str, &hints, &res);

			if (gaierr != 0 || res == NULL) {
				printf("[GW] DNS resolve '%s' failed: %d\n", peer_host, gaierr);
				gw_error_set(GW_ERR_TCP_COMM);
				netmgr_notify_tcp_connect_fail();
				zsock_close(cfd);
				k_msleep(CONFIG_SMARTGATEWAY_TCP_RECONNECT_MS);
				continue;
			}
			printf("[GW] DNS TCP connect -> %s:%u\n", peer_host, (unsigned)peer_port);
			if (zsock_connect(cfd, res->ai_addr, res->ai_addrlen) != 0) {
				printf("[GW] DNS connect %s:%u errno=%d\n",
				       peer_host, (unsigned)peer_port, errno);
				gw_error_set(GW_ERR_TCP_COMM);
				netmgr_notify_tcp_connect_fail();
				zsock_freeaddrinfo(res);
				zsock_close(cfd);
				k_msleep(CONFIG_SMARTGATEWAY_TCP_RECONNECT_MS);
				continue;
			}
			zsock_freeaddrinfo(res);
		}
#else
		{
			struct sockaddr_in peer = { 0 };

			peer.sin_family = AF_INET;
			peer.sin_port = htons(peer_port);
			if (zsock_inet_pton(AF_INET, peer_host, &peer.sin_addr) != 1) {
				printf("[GW] TCP peer IP parse failed: %s\n", peer_host);
				zsock_close(cfd);
				k_msleep(CONFIG_SMARTGATEWAY_TCP_RECONNECT_MS);
				continue;
			}
			printf("[GW] TCP connect -> %s:%u (active iface, NVS/gw peer)\n",
			       peer_host, (unsigned)peer_port);
			if (zsock_connect(cfd, (struct sockaddr *)&peer, sizeof(peer)) != 0) {
				printf("[GW] connect %s:%u errno=%d\n",
				       peer_host, (unsigned)peer_port, errno);
				gw_error_set(GW_ERR_TCP_COMM);
				netmgr_notify_tcp_connect_fail();
				zsock_close(cfd);
				k_msleep(CONFIG_SMARTGATEWAY_TCP_RECONNECT_MS);
				continue;
			}
		}
#endif

		netmgr_notify_tcp_connect_ok();
		gw_handle_client(cfd);
		zsock_close(cfd);
		k_msleep(CONFIG_SMARTGATEWAY_TCP_RECONNECT_MS);
	}
}

int tcp_gateway_task_start(void)
{
	/* prio 6: RS-232 Modbus txrx는 블로킹 — ADC(3)·UDP(4)보다 낮게 */
	k_tid_t t = k_thread_create(&tcp_gw_thr, tcp_gw_stack,
				    K_THREAD_STACK_SIZEOF(tcp_gw_stack), tcp_gateway_task, NULL,
				    NULL, NULL, 6, 0, K_NO_WAIT);

	if (t == NULL) {
		return -1;
	}
	k_thread_name_set(t, "tcp_gw_client");
	return 0;
}