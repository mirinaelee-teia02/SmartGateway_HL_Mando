/*
 * Smart Gateway — TCP ↔ RS-232 Modbus 게이트웨이
 *
 * 규격(4가지):
 *   ① 보드→서버(최초 연결): STX + MsgType + Seq + Body + ETX — Len·Error 없음 (0x80 CONNECT).
 *      Body = 마스터(ASCII) + 버전 헥사; Kconfig 기본 + 선택 시 SD FAT 텍스트 덮어쓰기.
 *   ② 서버→보드(①에 대한 응답): STX + MsgType + Seq + Timestamp + ETX — Timestamp=7B(time_helper).
 *   ③ 서버→보드(이후 주기 등): STX + Length(2,BE) + MsgType + Seq + Body + ETX — Tail Error 없음.
 *   ④ 보드→서버(RS-232/Modbus 결과): STX + Len_BE + MsgType + Seq + Body + Error + ETX (0x81).
 */

#include "tcp_gateway.h"
#include "rs232.h"
#include "sync_gate.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/autoconf.h>
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

/* --- 외곽 프레임 / MsgType 상수 --- */

#define GW_STX			  		0x55	/* 프레임 시작 바이트 */
#define GW_ETX			  		0x03	/* 프레임 종료 바이트 */
/* ② 핸드셰이크 서버→보드: STX + MsgType + Seq + Body + ETX */
#define GW_SRV_RX_HDR3_LEN	  	3U		/* STX(1)+MsgType(1)+Seq(1) */
#define GW_SRV_RX_TAIL_LEN	  	1U		/* ETX(1) */
/* ③ 세션 서버→보드: STX + Len(2,BE) + MsgType + Seq + Body + ETX */
#define GW_SRV_SESS_RX_HDR_LEN	  	5U		/* STX(1)+Len(2)+MsgType(1)+Seq(1) */
#define GW_SRV_SESS_RX_TAIL_LEN	  	1U		/* ETX(1) */
#define GW_TIMESTAMP_PAYLOAD_LEN  	13U		/* ② 서버 Timestamp: 시각 7B + RS-232 설정 6B */
#define GW_RS232_CFG_BODY_OFFSET	7U		/* TIMESYNC Body 내 RS-232 설정 시작 오프셋 */
/* 보드→서버: STX + Len_BE + MsgType + Seq + Body + Error + ETX */
#define GW_TX_HDR_LEN		  	5U		/* STX(1)+Len(2)+MsgType(1)+Seq(1) */
#define GW_TX_TAIL_LEN		  	2U		/* Error(1)+ETX(1) */
#define GW_MSG_TIMESYNC			0x00	/* TIMESYNC: 시각 동기 + RS-232 설정 수신 */
#define GW_MSG_REQUEST			0x01	/* REQUEST: Body = Modbus RTU 요청 */
#define GW_MSG_RESPONSE			0x82	/* RESPONSE: Body = Modbus RTU 응답 (구 0x81) */
#define GW_MSG_RS232_CFG_RESP	0x81	/* RS232-CFG RESP: 0x00 TIMESYNC 수신 후 설정 결과 */
#define GW_MSG_CONNECT		  	0x80	/* CONNECT: 핸드셰이크 송신용 */
#define GW_ERR_RS232_TIMEOUT	0xE3	/* 세션 응답 Tail Error (UART 실패) */
#define GW_BODY_CAP		  		256		/* 정적 버퍼(gw_mb_resp 등) 최대 Body 바이트 */
#define GW_MSG_CONN_REQ		  	0x70	/* 클라이언트→보드: 연결 요청(탈취 여부) */
#define GW_CONN_REQ_BODY_LEN	11		/* 0x70 Body 고정 길이 */
#define GW_CONN_REQ_FRAME_LEN	(GW_SRV_RX_HDR3_LEN + GW_CONN_REQ_BODY_LEN + GW_SRV_RX_TAIL_LEN) /* 15B */

/* 강제 탈취 Body 패턴 (11바이트) — 서버 모드 전용 */
#if !IS_ENABLED(CONFIG_SMARTGATEWAY_TCP_CLIENT_MODE)
static const uint8_t gw_takeover_body[GW_CONN_REQ_BODY_LEN] = {
	0x1F, 0x2F, 0x3F, 0x4F, 0x5F, 0x6F, 0x7F, 0x8F, 0x9F, 0xAF, 0xFF
};
#endif

BUILD_ASSERT(CONFIG_SMARTGATEWAY_TCP_MAX_BODY <= GW_BODY_CAP);
#define GW_MAX_BODY CONFIG_SMARTGATEWAY_TCP_MAX_BODY /* Kconfig: 프레임 Body 최대 허용 */

BUILD_ASSERT(CONFIG_SMARTGATEWAY_TCP_STREAM_BUF >=
	     (GW_TX_HDR_LEN + GW_MAX_BODY + GW_TX_TAIL_LEN));

/* 클라이언트 모드: 단일 스레드 */
#if IS_ENABLED(CONFIG_SMARTGATEWAY_TCP_CLIENT_MODE)
#define TCP_GW_STACK 6144
K_THREAD_STACK_DEFINE(tcp_gw_stack, TCP_GW_STACK);
static struct k_thread tcp_gw_thr;
#else
/* 서버 모드: accept 스레드 + session 스레드 분리 */
#define TCP_GW_ACCEPT_STACK  2048
#define TCP_GW_SESSION_STACK 6144
K_THREAD_STACK_DEFINE(tcp_gw_accept_stk, TCP_GW_ACCEPT_STACK);
K_THREAD_STACK_DEFINE(tcp_gw_session_stk, TCP_GW_SESSION_STACK);
static struct k_thread tcp_gw_accept_thr;
static struct k_thread tcp_gw_session_thr;
#endif

/* TCP 수신 누적 버퍼: 한 번에 오지 않는 프레임을 맞추기 위해 append 후 파싱 */
static uint8_t gw_rx_buf[CONFIG_SMARTGATEWAY_TCP_STREAM_BUF];
/* RS-232(Modbus RTU) 슬레이브 응답 임시 보관 → 0x82 Body로 재포장 */
static uint8_t gw_mb_resp[GW_BODY_CAP];
/* 송신: ① compact(3+N+1) 또는 ④ 긴 프레임(5+N+2) — 버퍼는 ④ 기준 */
static uint8_t gw_tx_frame[GW_TX_HDR_LEN + GW_BODY_CAP + GW_TX_TAIL_LEN];

/** 0x80(CONNECT) Body: gw_init_connect_body_once() */
static uint8_t gw_connect_body[GW_BODY_CAP];
static uint16_t gw_connect_body_len;

/* 서버 모드 공유 상태: accept 스레드 ↔ session 스레드 */
#if !IS_ENABLED(CONFIG_SMARTGATEWAY_TCP_CLIENT_MODE)
#define GW_CFD_NONE    (-1) /* 세션 없음 */
#define GW_CFD_PENDING (-2) /* 탈취 완료·새 세션 대기 중 (이 기간 신규 연결 거부) */
static int gw_active_cfd  = GW_CFD_NONE;
static int gw_pending_cfd = GW_CFD_NONE;
static K_MUTEX_DEFINE(gw_fd_mtx);
static K_SEM_DEFINE(gw_pending_sem, 0, 1);
static volatile bool gw_session_should_stop;   /* accept → session: 강제 종료 */
static volatile bool gw_session_paused;        /* accept → session: 일시 중지 (0x70 평가 중) */
static volatile bool gw_session_establishing;  /* session: 5초 대기 중 — 신규 연결 모두 거부 */
#endif

/** 마스터 문자열을 바이트열로 복사(NUL 제외). 반환: 복사한 바이트 수(최대 dst_cap) */
static size_t gw_copy_master_ascii(const char *src, uint8_t *dst, size_t dst_cap)
{
	size_t i = 0;

	for (; src[i] != '\0' && i < dst_cap; i++) {
		dst[i] = (uint8_t)src[i];
	}
	return i;
}

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

	if (done) {
		return;
	}
	done = true;

	char master[128];
	char ver_hex[48];

	strncpy(master, CONFIG_SMARTGATEWAY_LINE_ID, sizeof(master) - 1);
	master[sizeof(master) - 1] = '\0';
	strncpy(ver_hex, CONFIG_SMARTGATEWAY_TCP_CONNECT_VERSION_HEX, sizeof(ver_hex) - 1);
	ver_hex[sizeof(ver_hex) - 1] = '\0';

	size_t n_m = gw_copy_master_ascii(master, gw_connect_body, GW_BODY_CAP);

	if (n_m == 0U) {
		printf("[GW] CONNECT: empty master string\n");
	}

	int n_v = -1;

	if (n_m < GW_BODY_CAP) {
		n_v = gw_parse_hex_into(ver_hex, gw_connect_body + n_m, GW_BODY_CAP - n_m);
	}
	if (n_v < 0) {
		printf("[GW] CONNECT: invalid SMARTGATEWAY_TCP_CONNECT_VERSION_HEX\n");
		n_v = 0;
	}

	unsigned total = (unsigned)n_m + (unsigned)n_v;

	if (total == 0U || total > GW_MAX_BODY) {
		printf("[GW] CONNECT body empty/overflow (len=%u) -> fallback 02 00 00\n", total);
		gw_connect_body[0] = 0x02;
		gw_connect_body[1] = 0x00;
		gw_connect_body[2] = 0x00;
		gw_connect_body_len = 3;
	} else {
		gw_connect_body_len = (uint16_t)total;
	}
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
 * 반환 -1: 동기 오류(STX 아님·0x00 Timestamp ETX 불일치·Body 과대 등) → 보통 STX 1바이트 스킵.
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

	if (p[1] == GW_MSG_TIMESYNC) {
		*bl_out = (uint16_t)GW_TIMESTAMP_PAYLOAD_LEN;
		*need_out = GW_SRV_RX_HDR3_LEN + (size_t)GW_TIMESTAMP_PAYLOAD_LEN + GW_SRV_RX_TAIL_LEN;
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

/** ④ 보드→서버(Modbus 응답): STX + Len_BE + MsgType + Seq + Body + Error + ETX */
static int gw_build_tx_frame(uint8_t *out, size_t out_cap, uint8_t msg_type, uint8_t seq,
			     const uint8_t *body, uint16_t body_len, uint8_t err_code)
{
	const size_t need = GW_TX_HDR_LEN + body_len + GW_TX_TAIL_LEN;

	if (out_cap < need || body_len > GW_MAX_BODY) {
		return -EINVAL;
	}
	if (body_len > 0U && body == NULL) {
		return -EINVAL;
	}
	out[0] = GW_STX;
	sys_put_be16(body_len, out + 1);
	out[3] = msg_type;
	out[4] = seq;
	if (body_len > 0U) {
		memcpy(out + GW_TX_HDR_LEN, body, body_len);
	}
	out[GW_TX_HDR_LEN + body_len] = err_code;
	out[GW_TX_HDR_LEN + body_len + 1U] = GW_ETX;
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

/**
 * 핸드셰이크 수신: ② STX+MsgType+Seq+Timestamp(7B)+ETX. 0x00 한 번 처리 시 true.
 */
/**
 * 0x00 TIMESYNC Body[7..12] RS-232 설정 적용 후 0x81 결과 응답 전송.
 * rs232_reconfigure() 최대 3회(2초 간격) 내부 처리.
 */
static void gw_apply_rs232_cfg(int cfd, const uint8_t *body, size_t body_len, uint8_t seq)
{
	if (body == NULL || body_len < GW_RS232_CFG_BODY_OFFSET + 6U) {
		printf("[GW] 0x00 TIMESYNC RS-232 설정 미포함 (body_len=%u)\n", (unsigned)body_len);
		return;
	}

	rs232_remote_cfg_t cfg = {
		.protocol = body[GW_RS232_CFG_BODY_OFFSET + 0],
		.bps      = body[GW_RS232_CFG_BODY_OFFSET + 1],
		.data_bit = body[GW_RS232_CFG_BODY_OFFSET + 2],
		.stop_bit = body[GW_RS232_CFG_BODY_OFFSET + 3],
		.parity   = body[GW_RS232_CFG_BODY_OFFSET + 4],
		.flow     = body[GW_RS232_CFG_BODY_OFFSET + 5],
	};

	int ret = rs232_reconfigure(0, &cfg);
	uint8_t resp_byte = (ret == 0) ? 0x00 : 0x01;

	printf("[GW] RS-232 재설정 %s — 0x81 응답 전송\n", (ret == 0) ? "OK" : "NG");

	int plen = gw_build_compact_tx(gw_tx_frame, sizeof(gw_tx_frame),
				       GW_MSG_RS232_CFG_RESP, seq, &resp_byte, 1);

	if (plen > 0) {
		gw_hex_line("SmartGateway->Client", gw_tx_frame, (size_t)plen);
		if (gw_send_all(cfd, gw_tx_frame, (size_t)plen, "rs232cfg") != 0) {
			printf("[GW] 0x81 RS232-CFG 응답 전송 실패\n");
		}
	}
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

		ARG_UNUSED(seq);

		gw_hex_line("SmartGateway<-Server", rbuf, need);
		if (msg_type == GW_MSG_TIMESYNC) {
			sg_timesync_from_tcp_notify(body, bl);
			gw_apply_rs232_cfg(cfd, body, bl, seq); /* RS-232 재설정 + 0x81 응답 */
			got_sync = true;
		} else {
			printf("[GW] handshake skip MsgType 0x%02x (wait 0x00)\n", msg_type);
		}

		memmove(rbuf, rbuf + need, *rlen - need);
		*rlen -= need;
	}
	return got_sync;
}

/**
 * 클라이언트가 TCP 접속 직후 보내는 0x70 연결 요청 프레임 수신. (서버 모드 전용)
 * 프레임: STX(55) + 0x70 + 0x00 + Body(11B) + ETX(03) = 15바이트 고정.
 *
 * Body 패턴:
 *   비탈취: 0x01×11 — 기존 활성 연결이 있으면 신규 연결 거부
 *   탈취:   0x1F,0x2F,...,0xFF — 기존 활성 연결을 강제 종료 후 신규 수락
 *
 * @return  1: 강제 탈취, 0: 비탈취, -1: 수신/파싱 오류
 */
#if !IS_ENABLED(CONFIG_SMARTGATEWAY_TCP_CLIENT_MODE)
static int gw_recv_conn_req(int cfd, const char *client_ip)
{
	uint8_t buf[GW_CONN_REQ_FRAME_LEN];
	size_t n = 0;

	struct zsock_pollfd pfd = { .fd = cfd, .events = ZSOCK_POLLIN };
	/* 0x70 수신 타임아웃: 2초 (빠른 거부로 accept 루프 블로킹 최소화) */
	int64_t deadline = k_uptime_get() + 2000;

	while (n < GW_CONN_REQ_FRAME_LEN) {
		int remain_ms = (int)(deadline - k_uptime_get());

		if (remain_ms <= 0) {
			printf("[GW] [%s] 0x70 recv timeout (%u/%u B)\n",
			       client_ip, (unsigned)n, (unsigned)GW_CONN_REQ_FRAME_LEN);
			return -1;
		}
		int pr = zsock_poll(&pfd, 1, remain_ms);

		if (pr <= 0 || !(pfd.revents & ZSOCK_POLLIN)) {
			printf("[GW] [%s] 0x70 poll timeout/err\n", client_ip);
			return -1;
		}
		ssize_t r = zsock_recv(cfd, buf + n, GW_CONN_REQ_FRAME_LEN - n, 0);

		if (r <= 0) {
			printf("[GW] [%s] 0x70 recv closed/err r=%zd\n", client_ip, r);
			return -1;
		}
		n += (size_t)r;
	}

	gw_hex_line("SmartGateway<-Client(0x70)", buf, n);

	if (buf[0] != GW_STX || buf[1] != GW_MSG_CONN_REQ ||
	    buf[GW_CONN_REQ_FRAME_LEN - 1U] != GW_ETX) {
		printf("[GW] [%s] 0x70 frame invalid STX=%02x Type=%02x ETX=%02x\n",
		       client_ip, buf[0], buf[1], buf[GW_CONN_REQ_FRAME_LEN - 1U]);
		return -1;
	}

	const uint8_t *body = buf + GW_SRV_RX_HDR3_LEN;
	bool takeover = (memcmp(body, gw_takeover_body, GW_CONN_REQ_BODY_LEN) == 0);

	printf("[GW] [%s] 0x70 conn_req: %s\n", client_ip, takeover ? "강제 탈취" : "비탈취");
	return takeover ? 1 : 0;
}
#endif /* !CONFIG_SMARTGATEWAY_TCP_CLIENT_MODE */

/**
 * 핸드셰이크: 0x70 수신 완료 후 0x80(CONNECT) 1회 전송 → 0x00(TIMESYNC) 수신 대기.
 * (기존 2초 반복 전송 제거 — 0x70 선행 수신으로 연결 의사가 확인되었으므로 1회면 충분)
 */
static bool gw_tcp_handshake(int cfd, uint8_t *rbuf, size_t *rlen)
{
	/* 0x80 CONNECT 1회 전송 */
	int plen = gw_build_compact_tx(gw_tx_frame, sizeof(gw_tx_frame), GW_MSG_CONNECT, 0,
				       gw_connect_body, gw_connect_body_len);

	if (plen < 0 || gw_send_all(cfd, gw_tx_frame, (size_t)plen, "hs") != 0) {
		printf("[GW] 0x80 CONNECT 전송 실패\n");
		return false;
	}
	gw_hex_line("SmartGateway->Client", gw_tx_frame, (size_t)plen);

	/* 0x00 TIMESYNC 수신 대기 (재전송 없음) */
	struct zsock_pollfd pfd = { .fd = cfd, .events = ZSOCK_POLLIN };

	for (;;) {
		if (gw_session_should_stop) {
			return false; /* 탈취 신호 — 즉시 종료 */
		}
		int pr = zsock_poll(&pfd, 1, CONFIG_SMARTGATEWAY_TCP_HANDSHAKE_POLL_MS);

		if (gw_session_should_stop) {
			return false;
		}
		if (pr < 0) {
			printf("[GW] handshake poll errno=%d\n", errno);
			return false;
		}
		if (pr == 0) {
			continue; /* 0x00 대기 중 — 재전송 없이 계속 대기 */
		}
		if (pfd.revents & (ZSOCK_POLLHUP | ZSOCK_POLLERR)) {
			printf("[GW] handshake peer closed\n");
			return false;
		}
		if (!(pfd.revents & ZSOCK_POLLIN)) {
			continue;
		}
		if (*rlen >= sizeof(gw_rx_buf)) {
			*rlen = 0;
		}
		ssize_t n = zsock_recv(cfd, rbuf + *rlen, sizeof(gw_rx_buf) - *rlen, 0);

		if (n == 0) {
			printf("[GW] handshake peer closed\n");
			return false;
		}
		if (n < 0) {
			printf("[GW] handshake recv errno=%d\n", errno);
			return false;
		}
		*rlen += (size_t)n;
		if (gw_process_frames_handshake(cfd, rbuf, rlen)) {
			return true;
		}
	}
}

/**
 * 단일 TCP 클라이언트 세션.
 * 1) gw_tcp_handshake (0x80↔0x00 끝날 때까지 블록)
 * 2) 이후 for(;;): blocking recv → rlen 누적 → 내부 while에서 프레임 슬라이스
 *
 * 세션 수신(③): STX+Len_BE+MsgType+Seq+Body+ETX.
 * MsgType: 0x00 TIMESYNC / 0x01 REQUEST(Modbus RTU Body) 등.
 *
 * bl > MAX_BODY 이면 rlen=0 리셋.
 * 파싱 실패(reason 로그)는 바이트 하나만 밀어서 재동기(앞선 STX가 노이즈였을 수 있음).
 */
static void gw_handle_client(int cfd)
{
	uint8_t *const rbuf = gw_rx_buf; 
	size_t rlen = 0;		 

	gw_init_connect_body_once();

	/* ① Client→GW: 0x70 수신 (서버 루프에서 처리 완료)
	 * ② GW→Client: 0x80 CONNECT 1회 | ③ Client→GW: 0x00 TIMESYNC | ④ 응답 */
	if (!gw_tcp_handshake(cfd, rbuf, &rlen)) {
		printf("[GW] handshake failed\n");
		return;
	}

	// 서버의 데이터를 계속 받는 작업
	for (;;) {
		if (gw_session_should_stop) {
			printf("[GW] 세션 강제 종료 신호 수신\n");
			break;
		}

		/* 일시 중지: 2번 클라이언트 0x70 평가 중 — 새 RS-232 처리 보류 */
		if (gw_session_paused) {
			k_msleep(50);
			continue;
		}

		if (rlen >= sizeof(gw_rx_buf)) {
			printf("[GW] TCP rx buffer full — reset\n");
			rlen = 0;
		}

		/* 블로킹 recv 대신 300ms poll → 플래그 재확인 (Zephyr shutdown 미지원 대비) */
		struct zsock_pollfd mpfd = { .fd = cfd, .events = ZSOCK_POLLIN };
		int pr = zsock_poll(&mpfd, 1, 300);

		if (gw_session_should_stop) {
			printf("[GW] 세션 강제 종료 신호 수신\n");
			break;
		}
		if (pr < 0) {
			printf("[GW] recv poll errno=%d\n", errno);
			break;
		}
		if (pr == 0) {
			continue; /* 300ms timeout — 플래그 재확인 */
		}
		if (mpfd.revents & (ZSOCK_POLLHUP | ZSOCK_POLLERR)) {
			printf("[GW] peer closed\n");
			break;
		}
		if (!(mpfd.revents & ZSOCK_POLLIN)) {
			continue;
		}

		ssize_t n = zsock_recv(cfd, rbuf + rlen, sizeof(gw_rx_buf) - rlen, 0);
		if (n == 0) {
			printf("[GW] peer closed\n");
			break;
		}
		if (n < 0) {
			printf("[GW] recv errno=%d\n", errno);
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

			/* 0x00 TIMESYNC는 compact 포맷(②)으로도 올 수 있음:
			 * STX(1)+MsgType(1)+Seq(1)+Body(13)+ETX(1) = 17B — Len 필드 없음 */
#define GW_TIMESYNC_COMPACT_LEN \
	(GW_SRV_RX_HDR3_LEN + GW_TIMESTAMP_PAYLOAD_LEN + GW_SRV_RX_TAIL_LEN)
			if (rbuf[1] == GW_MSG_TIMESYNC &&
			    rlen >= GW_TIMESYNC_COMPACT_LEN &&
			    rbuf[GW_TIMESYNC_COMPACT_LEN - 1U] == GW_ETX) {
				uint8_t ts_seq  = rbuf[2];
				const uint8_t *ts_body = rbuf + GW_SRV_RX_HDR3_LEN;

				gw_hex_line("SmartGateway<-Client (compact TIMESYNC)", rbuf,
					    GW_TIMESYNC_COMPACT_LEN);
				sg_timesync_from_tcp_notify(ts_body, GW_TIMESTAMP_PAYLOAD_LEN);
				gw_apply_rs232_cfg(cfd, ts_body, GW_TIMESTAMP_PAYLOAD_LEN, ts_seq);
				memmove(rbuf, rbuf + GW_TIMESYNC_COMPACT_LEN,
					rlen - GW_TIMESYNC_COMPACT_LEN);
				rlen -= GW_TIMESYNC_COMPACT_LEN;
				continue;
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

			if (msg_type == GW_MSG_TIMESYNC) {
				gw_hex_line("SmartGateway<-Server", rbuf, need);
				sg_timesync_from_tcp_notify(body, bl);
				gw_apply_rs232_cfg(cfd, body, bl, seq); /* RS-232 재설정 + 0x81 응답 */
				memmove(rbuf, rbuf + need, rlen - need);
				rlen -= need;
				continue;
			}
			if (msg_type != GW_MSG_REQUEST) {
				printf("[GW] skip MsgType 0x%02x (not 0x01)\n", msg_type);
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

			/*
			 * TIMESYNC에서 설정된 프로토콜이 Modbus ASCII(0x02)이면
			 * Body 구조: [COMNum(1B)] + [ASCII 프레임: ':' ... '\r\n']
			 * COM 포트 번호를 분리해 해당 포트로 ASCII 프레임 전송,
			 * 응답 앞에 COM 포트 번호를 다시 붙여 반환.
			 * RTU(0x01) 등 다른 프로토콜은 기존 Body 그대로 처리.
			 */
			bool ascii_pt = (rs232_get_protocol(0) == 0x02U && bl >= 2U);

			uint8_t  use_port  = ascii_pt ? body[0] : 0U;
			const uint8_t *use_body = ascii_pt ? (body + 1U) : body;
			uint16_t use_bl    = ascii_pt ? (uint16_t)(bl - 1U) : bl;

			/* ASCII 패스스루 응답은 gw_mb_resp[1]부터 채움 → [0]에 COM 포트 예약 */
			uint8_t  *resp_buf = ascii_pt ? (gw_mb_resp + 1U) : gw_mb_resp;
			size_t    resp_cap = ascii_pt ? (sizeof(gw_mb_resp) - 1U) : sizeof(gw_mb_resp);

			if (ascii_pt) {
				printf("[GW] ASCII passthrough: COM%u frame %u B\n",
				       use_port, use_bl);
			}

			size_t mb_len = 0;
			int mbr = rs232_modbus_txrx(use_port, use_body, use_bl,
						    resp_buf, resp_cap, &mb_len);

			if (ascii_pt && mbr == 0) {
				/* 응답 앞에 COM 포트 번호 삽입 */
				gw_mb_resp[0] = use_port;
				mb_len += 1U;
			}

			if (mbr != 0) {
				printf("[GW] RS-232 fail ret=%d rx_len=%u -> 0x82 Err=0x%02x Body 0\n", mbr,
				       (unsigned)mb_len, GW_ERR_RS232_TIMEOUT);
				int olen = gw_build_tx_frame(gw_tx_frame, sizeof(gw_tx_frame),
							     GW_MSG_RESPONSE, seq, NULL, 0,
							     GW_ERR_RS232_TIMEOUT);

				if (olen > 0) {
					gw_hex_line("SmartGateway->Server", gw_tx_frame, (size_t)olen);
					if (gw_send_all(cfd, gw_tx_frame, (size_t)olen, "err") != 0) {
						printf("[GW] SmartGateway->Server err response send fail\n");
					}
				} else {
					printf("[GW] gw_build_tx_frame(err) fail olen=%d\n", olen);
				}
			} else {
				if (!ascii_pt) {
					gw_print_modbus_fc03_like(gw_mb_resp, mb_len);
				}
				int olen = gw_build_tx_frame(gw_tx_frame, sizeof(gw_tx_frame),
							     GW_MSG_RESPONSE, seq, gw_mb_resp,
							     (uint16_t)mb_len, 0);

				if (olen > 0) {
					gw_hex_line("SmartGateway->Server", gw_tx_frame, (size_t)olen);
					if (gw_send_all(cfd, gw_tx_frame, (size_t)olen, "ok") != 0) {
						printf("[GW] SmartGateway->Server ok response send fail\n");
					}
				} else {
					printf("[GW] gw_build_tx_frame(ok) fail olen=%d mb_len=%u cap=%zu\n",
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
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

#if IS_ENABLED(CONFIG_SMARTGATEWAY_TCP_CLIENT_MODE)
	for (;;) {
		int cfd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

		if (cfd < 0) {
			printf("[GW] socket() 실패\n");
			k_msleep(CONFIG_SMARTGATEWAY_TCP_RECONNECT_MS);
			continue;
		}

		struct sockaddr_in peer = { 0 };

		peer.sin_family = AF_INET;
		peer.sin_port = htons((uint16_t)CONFIG_SMARTGATEWAY_TCP_PEER_PORT);
		if (zsock_inet_pton(AF_INET, CONFIG_SMARTGATEWAY_TCP_PEER_IP, &peer.sin_addr) != 1) {
			printf("[GW] TCP_PEER_IP 변환 실패: %s\n", CONFIG_SMARTGATEWAY_TCP_PEER_IP);
			zsock_close(cfd);
			k_msleep(CONFIG_SMARTGATEWAY_TCP_RECONNECT_MS);
			continue;
		}

		printf("[GW] TCP connect -> %s:%u (RX: STX+Len+Msg+Seq+Body+ETX / TX: 5+%u+2)\n",
		       CONFIG_SMARTGATEWAY_TCP_PEER_IP,
		       (unsigned)CONFIG_SMARTGATEWAY_TCP_PEER_PORT, (unsigned)GW_MAX_BODY);

		if (zsock_connect(cfd, (struct sockaddr *)&peer, sizeof(peer)) != 0) {
			printf("[GW] connect %s:%u errno=%d\n", CONFIG_SMARTGATEWAY_TCP_PEER_IP,
			       (unsigned)CONFIG_SMARTGATEWAY_TCP_PEER_PORT, errno);
			zsock_close(cfd);
			k_msleep(CONFIG_SMARTGATEWAY_TCP_RECONNECT_MS);
			continue;
		}

		gw_handle_client(cfd);
		zsock_close(cfd);
		k_msleep(CONFIG_SMARTGATEWAY_TCP_RECONNECT_MS);
	}
#endif
}

/* ============================================================
 * 서버 모드 전용: accept 스레드 + session 스레드
 * ============================================================
 * accept 스레드: 항상 listen → 0x70 수신 → 탈취 판별 → session 스레드에 fd 인계
 * session 스레드: fd 인계받아 0x80/0x00 핸드셰이크 후 0x01/0x81 통신 처리
 *
 * 탈취 시: accept 스레드가 gw_active_cfd 를 닫으면 session 스레드의
 *           zsock_recv() 가 에러 반환 → gw_handle_client() 자연 종료
 * ============================================================ */
#if !IS_ENABLED(CONFIG_SMARTGATEWAY_TCP_CLIENT_MODE)

static void gw_accept_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	int fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd < 0) {
		printf("[GW] socket() 실패\n");
		return;
	}

	int one = 1;
	(void)zsock_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	struct sockaddr_in sin = { 0 };
	sin.sin_family = AF_INET;
	sin.sin_port   = htons((uint16_t)CONFIG_SMARTGATEWAY_ETH_TCP_LISTEN_PORT);
	if (zsock_inet_pton(AF_INET, CONFIG_SMARTGATEWAY_ETH_TCP_BIND_IP, &sin.sin_addr) != 1) {
		printf("[GW] TCP_BIND_IP 변환 실패: %s\n", CONFIG_SMARTGATEWAY_ETH_TCP_BIND_IP);
		zsock_close(fd);
		return;
	}
	if (zsock_bind(fd, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
		printf("[GW] bind %s:%u errno=%d\n",
		       CONFIG_SMARTGATEWAY_ETH_TCP_BIND_IP,
		       (unsigned)CONFIG_SMARTGATEWAY_ETH_TCP_LISTEN_PORT, errno);
		zsock_close(fd);
		return;
	}
	if (zsock_listen(fd, 1) != 0) {
		printf("[GW] listen errno=%d\n", errno);
		zsock_close(fd);
		return;
	}

	printf("[GW] TCP listen %s:%u\n",
	       CONFIG_SMARTGATEWAY_ETH_TCP_BIND_IP,
	       (unsigned)CONFIG_SMARTGATEWAY_ETH_TCP_LISTEN_PORT);

	for (;;) {
		struct sockaddr_in peer_addr = { 0 };
		socklen_t peer_len = sizeof(peer_addr);
		int new_cfd = zsock_accept(fd, (struct sockaddr *)&peer_addr, &peer_len);
		if (new_cfd < 0) {
			k_msleep(100);
			continue;
		}

		/* 클라이언트 IP 문자열 변환 (로그 식별용) */
		char client_ip[INET_ADDRSTRLEN] = "?.?.?.?";
		zsock_inet_ntop(AF_INET, &peer_addr.sin_addr, client_ip, sizeof(client_ip));

		k_mutex_lock(&gw_fd_mtx, K_FOREVER);
		bool has_active = (gw_active_cfd >= 0);
		/* GW_CFD_PENDING: 탈취 완료·session thread 인수 전 / gw_session_establishing: 5초 대기 중
		 * 두 경우 모두 신규 연결 거부 */
		bool is_establishing = gw_session_establishing || (gw_active_cfd == GW_CFD_PENDING);
		k_mutex_unlock(&gw_fd_mtx);

		/* 탈취 처리 중 or 세션 초기화 대기 중(5초)이면 모든 신규 연결 즉시 거부 */
		if (is_establishing) {
			printf("[GW] [%s] 세션 초기화 중 — 연결 거부\n", client_ip);
			zsock_close(new_cfd);
			continue;
		}

		/* 활성 세션이 있으면 RS-232 즉시 중단 + 0x70 평가 동안 일시 중지 */
		if (has_active) {
			rs232_abort_txrx();              /* 진행 중 RS-232 즉시 중단 */
			gw_session_paused = true;
			printf("[GW] [%s] 새 연결 — RS-232 중단·세션 일시 중지, 0x70 평가 중\n", client_ip);
		} else {
			printf("[GW] [%s] 새 연결 수락\n", client_ip);
		}

		/* 0x70 연결 요청 수신 */
		int takeover = gw_recv_conn_req(new_cfd, client_ip);

		k_mutex_lock(&gw_fd_mtx, K_FOREVER);

		/* 0x70 오류: 거부 후 기존 세션 재개 */
		if (takeover < 0) {
			printf("[GW] [%s] 0x70 오류 — 연결 거부, 기존 세션 재개\n", client_ip);
			zsock_close(new_cfd);
			gw_session_paused = false;
			if (has_active) { rs232_abort_clear(); } /* 중단된 RS-232 플래그 리셋 */
			k_mutex_unlock(&gw_fd_mtx);
			continue;
		}

		/* 비탈취: 기존 세션 재개 후 거부 */
		if (takeover == 0 && gw_active_cfd != GW_CFD_NONE) {
			printf("[GW] [%s] 비탈취 — 기존 세션 재개, 신규 거부\n", client_ip);
			zsock_close(new_cfd);
			gw_session_paused = false;
			rs232_abort_clear(); /* 중단된 RS-232 플래그 리셋 */
			k_mutex_unlock(&gw_fd_mtx);
			continue;
		}

		/* 강제 탈취: 기존 실제 세션 fd 즉시 종료 */
		if (takeover == 1 && gw_active_cfd >= 0) {
			printf("[GW] [%s] 강제 탈취 — 기존 연결(fd=%d) 즉시 종료\n",
			       client_ip, gw_active_cfd);
			rs232_abort_txrx();              /* RS-232 수신 루프 즉시 중단 */
			gw_session_should_stop = true;
			zsock_shutdown(gw_active_cfd, ZSOCK_SHUT_RDWR);
		}
		/* 탈취 대기 중 재탈취: 기존 pending fd 교체 */
		if (takeover == 1 && gw_pending_cfd >= 0) {
			printf("[GW] [%s] 대기 중 재탈취 — pending fd=%d 닫기\n",
			       client_ip, gw_pending_cfd);
			zsock_close(gw_pending_cfd);
			gw_pending_cfd = GW_CFD_NONE;
		}
		if (takeover == 1) {
			gw_active_cfd = GW_CFD_PENDING; /* 신규 연결 진입 차단 */
		}

		/* session 스레드로 fd 인계 — 즉시 0x80 전송 */
		gw_pending_cfd = new_cfd;
		k_mutex_unlock(&gw_fd_mtx);
		k_sem_give(&gw_pending_sem);
	}
}

static void gw_session_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	for (;;) {
		/* accept 스레드가 줄 때까지 대기 */
		k_sem_take(&gw_pending_sem, K_FOREVER);

		k_mutex_lock(&gw_fd_mtx, K_FOREVER);
		int cfd = gw_pending_cfd;
		gw_pending_cfd = GW_CFD_NONE;
		gw_active_cfd  = cfd; /* GW_CFD_PENDING → 실제 fd로 교체 */
		/* 탈취 여부: accept 스레드가 stop 플래그를 세운 뒤 세마포어를 준 경우 */
		bool was_takeover = gw_session_should_stop;
		gw_session_should_stop = false;
		gw_session_paused = false;
		if (was_takeover) {
			gw_session_establishing = true; /* 뮤텍스 안에서 설정 — accept 경쟁 창 제거 */
		}
		k_mutex_unlock(&gw_fd_mtx);

		if (was_takeover) {
			/* RS-232 중단 플래그 초기화 후 5초 대기 — 이 기간 신규 연결 모두 거부됨 */
			rs232_abort_clear();
			printf("[GW] 탈취 완료 — 5초 대기 후 새 클라이언트(fd=%d)에 0x80 전송\n", cfd);
			k_msleep(5000);
			gw_session_establishing = false;
			printf("[GW] 5초 대기 완료 — 새 클라이언트(fd=%d)에 0x80 전송\n", cfd);
		}

		/* 0x80 CONNECT → 0x00 TIMESYNC(+RS-232 설정) → 0x81 설정 결과 → 0x01/0x82 통신 */
		gw_handle_client(cfd);

		/* 세션 종료 정리 */
		k_mutex_lock(&gw_fd_mtx, K_FOREVER);
		if (gw_active_cfd == cfd) {
			/* 정상 종료: 아직 active — 직접 닫기 */
			zsock_close(cfd);
			gw_active_cfd = GW_CFD_NONE;
		} else {
			/* 탈취: accept 스레드가 shutdown만 했으므로 여기서 close 완료 */
			zsock_close(cfd);
		}
		k_mutex_unlock(&gw_fd_mtx);
	}
}

#endif /* !CONFIG_SMARTGATEWAY_TCP_CLIENT_MODE */

int tcp_gateway_task_start(void)
{
#if IS_ENABLED(CONFIG_SMARTGATEWAY_TCP_CLIENT_MODE)
	k_tid_t t = k_thread_create(&tcp_gw_thr, tcp_gw_stack,
				    K_THREAD_STACK_SIZEOF(tcp_gw_stack), tcp_gateway_task, NULL,
				    NULL, NULL, 5, 0, K_NO_WAIT);
	if (t == NULL) {
		return -1;
	}
	k_thread_name_set(t, "tcp_gw_client");
#else
	/* 서버 모드: accept 스레드 + session 스레드 */
	k_tid_t ta = k_thread_create(&tcp_gw_accept_thr, tcp_gw_accept_stk,
				     K_THREAD_STACK_SIZEOF(tcp_gw_accept_stk),
				     gw_accept_thread_fn, NULL, NULL, NULL, 5, 0, K_NO_WAIT);
	if (ta == NULL) {
		return -1;
	}
	k_thread_name_set(ta, "tcp_gw_accept");

	k_tid_t ts = k_thread_create(&tcp_gw_session_thr, tcp_gw_session_stk,
				     K_THREAD_STACK_SIZEOF(tcp_gw_session_stk),
				     gw_session_thread_fn, NULL, NULL, NULL, 5, 0, K_NO_WAIT);
	if (ts == NULL) {
		return -1;
	}
	k_thread_name_set(ts, "tcp_gw_session");
#endif
	return 0;
}