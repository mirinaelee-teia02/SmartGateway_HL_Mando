
#include "rs232.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/autoconf.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/spinlock.h>
#include <zephyr/drivers/uart.h>
#include <stdbool.h>

/* TCP 탈취 등 외부에서 수신 루프를 즉시 중단시키는 플래그 */
static volatile bool rs232_abort_flag;

/* 포트별 현재 프로토콜: 0x00=RS-232(미사용), 0x01=Modbus RTU(기본), 0x02=Modbus ASCII */
static uint8_t rs232_protocol[RS232_MAX_PORTS];

void rs232_abort_txrx(void)  { rs232_abort_flag = true;  }
void rs232_abort_clear(void) { rs232_abort_flag = false; }

/* OR/FE/PE 등이 뜨면 드라이버가 RDRF를 안 올리는 경우가 있어, 대기 전·중에 주기적으로 비움 */
static void rs232_uart_clear_errors(const struct device *dev, bool log_if_any)
{
	int e = uart_err_check(dev);

	if (e != 0 && log_if_any) {
		printf("[RS232] UART err_check(cleared)=0x%x (OVERRUN=%d FRAMING=%d PARITY=%d NOISE=%d)\n",
		       e, (e & UART_ERROR_OVERRUN) ? 1 : 0, (e & UART_ERROR_FRAMING) ? 1 : 0,
		       (e & UART_ERROR_PARITY) ? 1 : 0, (e & UART_ERROR_NOISE) ? 1 : 0);
	}
}

/* 포트 0: MikroBUS FC4 — flexcomm4 / LPUART4 (P1_8 RX, P1_9 TX) */
#define RS232_0_NODE	DT_NODELABEL(flexcomm4_lpuart4)
/* 포트 1: MikroBUS J5 — flexcomm5 / LPUART5 (P1_16 RX, P1_17 TX) */
#define RS232_1_NODE	DT_NODELABEL(flexcomm5_lpuart5)

#define RS232_TASK_STACK 3072
#define RS232_UDP_RX_MAX 512
#define RS232_MODBUS_RX_MAX	256

#define RS232_MODBUS_RX_TMO_MS		CONFIG_SMARTGATEWAY_TCP_RS232_RX_TIMEOUT_MS
static const struct device *dev_by_port[RS232_MAX_PORTS];

/*
 * Modbus UART(포트 0): RX는 IRQ에서 링에 적재 → 응답 수신 루프에서 k_busy_wait 없이 k_msleep로 대기.
 * (짧은 busy-wait 반복은 TCP/네트워크 스레드를 굶겨 연속 트랜잭션 시 "먹통"처럼 보일 수 있음.)
 */
#if defined(CONFIG_UART_INTERRUPT_DRIVEN) && CONFIG_UART_INTERRUPT_DRIVEN
#define RS232_IRQ_RX_RING_SZ 512

/* 포트별 IRQ RX 링 버퍼 — 인덱스 0=UART0(flexcomm4), 1=UART1(flexcomm5) */
static uint8_t  rs232_irq_rx_ring[RS232_MAX_PORTS][RS232_IRQ_RX_RING_SZ];
static uint16_t rs232_irq_rx_head[RS232_MAX_PORTS];
static uint16_t rs232_irq_rx_tail[RS232_MAX_PORTS];
static struct k_spinlock rs232_irq_rx_lock[RS232_MAX_PORTS];

/* dev → 포트 인덱스 반환. 해당 없으면 -1 */
static int rs232_port_idx(const struct device *dev)
{
	for (int i = 0; i < RS232_MAX_PORTS; i++) {
		if (dev_by_port[i] != NULL && dev == dev_by_port[i]) {
			return i;
		}
	}
	return -1;
}

static void rs232_irq_rx_ring_reset(int idx)
{
	k_spinlock_key_t key = k_spin_lock(&rs232_irq_rx_lock[idx]);

	rs232_irq_rx_head[idx] = 0;
	rs232_irq_rx_tail[idx] = 0;
	k_spin_unlock(&rs232_irq_rx_lock[idx], key);
}

static void rs232_irq_rx_ring_push(int idx, const uint8_t *p, int len)
{
	k_spinlock_key_t key = k_spin_lock(&rs232_irq_rx_lock[idx]);

	for (int i = 0; i < len; i++) {
		uint16_t nh = (uint16_t)((rs232_irq_rx_head[idx] + 1U) % RS232_IRQ_RX_RING_SZ);

		if (nh == rs232_irq_rx_tail[idx]) {
			rs232_irq_rx_tail[idx] =
				(uint16_t)((rs232_irq_rx_tail[idx] + 1U) % RS232_IRQ_RX_RING_SZ);
		}
		rs232_irq_rx_ring[idx][rs232_irq_rx_head[idx]] = p[i];
		rs232_irq_rx_head[idx] = nh;
	}
	k_spin_unlock(&rs232_irq_rx_lock[idx], key);
}

static int rs232_irq_rx_ring_pop(int idx, uint8_t *out)
{
	k_spinlock_key_t key = k_spin_lock(&rs232_irq_rx_lock[idx]);

	if (rs232_irq_rx_head[idx] == rs232_irq_rx_tail[idx]) {
		k_spin_unlock(&rs232_irq_rx_lock[idx], key);
		return -EAGAIN;
	}
	*out = rs232_irq_rx_ring[idx][rs232_irq_rx_tail[idx]];
	rs232_irq_rx_tail[idx] =
		(uint16_t)((rs232_irq_rx_tail[idx] + 1U) % RS232_IRQ_RX_RING_SZ);
	k_spin_unlock(&rs232_irq_rx_lock[idx], key);
	return 0;
}

/* 포트 0/1 공용 ISR — dev_by_port[] 로 포트 인덱스 식별 */
static void rs232_uart_irq_isr(const struct device *dev, void *user_data)
{
	int idx = rs232_port_idx(dev);
	uint8_t chunk[16];

	ARG_UNUSED(user_data);

	if (idx < 0) {
		return;
	}
	(void)uart_irq_update(dev);
	while (uart_irq_rx_ready(dev) != 0) {
		int n = uart_fifo_read(dev, chunk, sizeof(chunk));

		if (n <= 0) {
			break;
		}
		rs232_irq_rx_ring_push(idx, chunk, n);
	}
}

static int rs232_rx_try_byte(const struct device *dev, uint8_t *ch)
{
	int idx = rs232_port_idx(dev);

	if (idx >= 0 && rs232_irq_rx_ring_pop(idx, ch) == 0) {
		return 0;
	}
	return uart_poll_in(dev, ch);
}
#else /* !CONFIG_UART_INTERRUPT_DRIVEN */

/* IRQ 미사용 시: poll 방식으로 직접 읽음 */
static int rs232_rx_try_byte(const struct device *dev, uint8_t *ch)
{
	return uart_poll_in(dev, ch);
}
#endif /* CONFIG_UART_INTERRUPT_DRIVEN */

static void rs232_hw_poll_drain(const struct device *dev)
{
	uint8_t ch;

	while (uart_poll_in(dev, &ch) == 0) {
		(void)ch;
	}
}

static void rs232_print_uart_kconfig(void)
{
	const char *par = "NONE";

#if defined(CONFIG_SMARTGATEWAY_RS232_PARITY_EVEN)
	par = "EVEN";
#elif defined(CONFIG_SMARTGATEWAY_RS232_PARITY_ODD)
	par = "ODD";
#endif
	const char *stop = "1";

#if defined(CONFIG_SMARTGATEWAY_RS232_STOP_BITS_2)
	stop = "2";
#endif
	const char *data = "8";

#if defined(CONFIG_SMARTGATEWAY_RS232_DATA_BITS_7)
	data = "7";
#endif
	const char *flow = "NONE";

#if defined(CONFIG_SMARTGATEWAY_RS232_FLOW_CTRL_RTS_CTS)
	flow = "RTS_CTS";
#endif

	printf("[RS232] Kconfig line: %u baud, data=%s, parity=%s, stop=%s, flow=%s\n",
	       CONFIG_SMARTGATEWAY_RS232_BAUD_RATE, data, par, stop, flow);
}

static int rs232_uart_setup(const struct device *dev)
{
	enum uart_config_parity parity = UART_CFG_PARITY_NONE;

#if defined(CONFIG_SMARTGATEWAY_RS232_PARITY_EVEN)
	parity = UART_CFG_PARITY_EVEN;
#elif defined(CONFIG_SMARTGATEWAY_RS232_PARITY_ODD)
	parity = UART_CFG_PARITY_ODD;
#endif

	enum uart_config_stop_bits stop_bits = UART_CFG_STOP_BITS_1;

#if defined(CONFIG_SMARTGATEWAY_RS232_STOP_BITS_2)
	stop_bits = UART_CFG_STOP_BITS_2;
#endif

	enum uart_config_data_bits data_bits = UART_CFG_DATA_BITS_8;

#if defined(CONFIG_SMARTGATEWAY_RS232_DATA_BITS_7)
	data_bits = UART_CFG_DATA_BITS_7;
#endif

	enum uart_config_flow_control flow_ctrl = UART_CFG_FLOW_CTRL_NONE;

#if defined(CONFIG_SMARTGATEWAY_RS232_FLOW_CTRL_RTS_CTS)
	flow_ctrl = UART_CFG_FLOW_CTRL_RTS_CTS;
#endif

	struct uart_config cfg = {
		.baudrate = CONFIG_SMARTGATEWAY_RS232_BAUD_RATE,
		.parity = (uint8_t)parity,
		.stop_bits = (uint8_t)stop_bits,
		.data_bits = (uint8_t)data_bits,
		.flow_ctrl = (uint8_t)flow_ctrl,
	};

	return uart_configure(dev, &cfg);
}

int rs232_reconfigure(uint8_t port_id, const rs232_remote_cfg_t *cfg)
{
	if (port_id >= RS232_MAX_PORTS || cfg == NULL) {
		return -1;
	}
	const struct device *dev = dev_by_port[port_id];

	if (dev == NULL) {
		return -1;
	}

	/* BPS 변환 */
	uint32_t baud;

	switch (cfg->bps) {
		case 0x00: baud = 9600;   break;
		case 0x01: baud = 115200; break;
		default:
			printf("[RS232] reconfigure: 알 수 없는 BPS 0x%02x\n", cfg->bps);
			return -1;
	}

	/* Data bit 변환 */
	enum uart_config_data_bits data_bits;

	switch (cfg->data_bit) {
		case 0x00: data_bits = UART_CFG_DATA_BITS_7; break;
		case 0x01: data_bits = UART_CFG_DATA_BITS_8; break;
		default:
			printf("[RS232] reconfigure: 알 수 없는 DataBit 0x%02x\n", cfg->data_bit);
			return -1;
	}

	/* Stop bit 변환 */
	enum uart_config_stop_bits stop_bits;

	switch (cfg->stop_bit) {
		case 0x00: stop_bits = UART_CFG_STOP_BITS_1; break;
		case 0x01: stop_bits = UART_CFG_STOP_BITS_2; break;
		default:
			printf("[RS232] reconfigure: 알 수 없는 StopBit 0x%02x\n", cfg->stop_bit);
			return -1;
	}

	/* Parity 변환 */
	enum uart_config_parity parity;

	switch (cfg->parity) {
		case 0x00: parity = UART_CFG_PARITY_NONE; break;
		case 0x01: parity = UART_CFG_PARITY_ODD;  break;
		case 0x02: parity = UART_CFG_PARITY_EVEN; break;
		default:
			printf("[RS232] reconfigure: 알 수 없는 Parity 0x%02x\n", cfg->parity);
			return -1;
	}

	/* Flow control 변환 */
	enum uart_config_flow_control flow_ctrl;

	switch (cfg->flow) {
		case 0x00: flow_ctrl = UART_CFG_FLOW_CTRL_NONE;     break;
		case 0x01: flow_ctrl = UART_CFG_FLOW_CTRL_RTS_CTS;  break;
		default:
			printf("[RS232] reconfigure: 알 수 없는 Flow 0x%02x\n", cfg->flow);
			return -1;
	}

	const char *proto_str = (cfg->protocol == 0x00) ? "RS232(미사용)" :
							(cfg->protocol == 0x01) ? "Modbus-RTU" :
							(cfg->protocol == 0x02) ? "Modbus-ASCII" : "Unknown";
	struct uart_config ucfg = {
		.baudrate  = baud,
		.parity    = (uint8_t)parity,
		.stop_bits = (uint8_t)stop_bits,
		.data_bits = (uint8_t)data_bits,
		.flow_ctrl = (uint8_t)flow_ctrl,
	};

	printf("[RS232] reconfigure 시도: %s %u baud, data=%d, stop=%d, parity=%d, flow=%d\n",
	       proto_str, baud, (int)cfg->data_bit, (int)cfg->stop_bit,
	       (int)cfg->parity, (int)cfg->flow);

	/* 최대 3회 시도 (실패 시 2초 대기, 중간에 abort 가능) */
	for (int attempt = 1; attempt <= 3; attempt++) {
		if (attempt > 1) {
			/* 2초 대기 — 100ms 단위로 abort 플래그 확인 */
			int64_t deadline = k_uptime_get() + 2000;

			while (k_uptime_get() < deadline) {
				if (rs232_abort_flag) {
					printf("[RS232] reconfigure: abort 신호 — 중단\n");
					return -1;
				}
				k_msleep(100);
			}
		}
		if (rs232_abort_flag) {
			return -1;
		}
		int ret = uart_configure(dev, &ucfg);

		if (ret == 0) {
#if defined(CONFIG_UART_INTERRUPT_DRIVEN) && CONFIG_UART_INTERRUPT_DRIVEN
			rs232_irq_rx_ring_reset((int)port_id); /* RX 링 버퍼 초기화 */
#endif
			rs232_uart_clear_errors(dev, false);
			rs232_protocol[port_id] = cfg->protocol; /* 프로토콜 저장 */
			printf("[RS232] reconfigure 성공 (시도 %d) — protocol=%s\n",
			       attempt, proto_str);
			return 0;
		}
		printf("[RS232] reconfigure 시도 %d 실패: ret=%d\n", attempt, ret);
	}

	printf("[RS232] reconfigure 3회 전체 실패 — NG\n");
	return -1;
}

static void rs232_ports_init(void)
{
	memset(dev_by_port, 0, sizeof(dev_by_port));
	/* 기본 프로토콜: Modbus RTU (rs232_reconfigure 호출 전 안전 기본값) */
	for (int i = 0; i < RS232_MAX_PORTS; i++) {
		rs232_protocol[i] = 0x01U;
	}
	/* 테스트 모드(DEBUG_CONSOLE=y): UART0 = 콘솔 → 포트 0 RS-232 초기화 건너뜀 */
#if !defined(CONFIG_SMARTGATEWAY_DEBUG_CONSOLE)
#if DT_NODE_EXISTS(RS232_0_NODE)
	dev_by_port[0] = DEVICE_DT_GET(RS232_0_NODE);
#endif
#endif /* !CONFIG_SMARTGATEWAY_DEBUG_CONSOLE */
#if DT_NODE_EXISTS(RS232_1_NODE)
	dev_by_port[1] = DEVICE_DT_GET(RS232_1_NODE);
#endif
}

int rs232_send(uint8_t port_id, const uint8_t *data, size_t len)
{
	const struct device *dev;

	if (!data || len == 0 || port_id >= RS232_MAX_PORTS) {
		return -EINVAL;
	}
	dev = dev_by_port[port_id];
	if (dev == NULL) {
		return -ENODEV;
	}
	for (size_t i = 0; i < len; i++) {
		uart_poll_out(dev, data[i]);
	}
	return 0;
}

int rs232_tx(uint8_t port_id, const uint8_t *data, size_t len)
{
	return rs232_send(port_id, data, len);
}

/**
 * RX 버스 비우기: 바이트가 오면 읽어 버리고, 연속 무음이 idle_ms 만족할 때까지(최대 cap_ms).
 * 이전 트랜잭션 잔여·지연 바이트로 다음 Modbus 프레임이 망가지는 것을 줄임.
 */
static void rs232_discard_until_rx_idle(const struct device *dev, uint32_t idle_ms, uint32_t cap_ms)
{
	const int64_t t_end = k_uptime_get() + (int64_t)cap_ms;
	int64_t idle_since = k_uptime_get();

	while (k_uptime_get() < t_end) {
		uint8_t ch;

		if (rs232_rx_try_byte(dev, &ch) == 0) {
			(void)ch;
			idle_since = k_uptime_get();
			while (rs232_rx_try_byte(dev, &ch) == 0) {
				(void)ch;
				idle_since = k_uptime_get();
			}
			continue;
		}
		if ((k_uptime_get() - idle_since) >= (int64_t)idle_ms) {
			return;
		}
		k_msleep(1);
	}
}

/* 드라이버 RX FIFO에 쌓인 바이트만 즉시 비움(무음 대기 없음). 에러 반환 직후·다음 TX 직전에 호출해
 * 이전 트랜잭션 잔여로 다음 요청/응답이 뒤틀리는 것을 막는다. */
static void rs232_drain_rx_fifo(const struct device *dev)
{
#if defined(CONFIG_UART_INTERRUPT_DRIVEN) && CONFIG_UART_INTERRUPT_DRIVEN
	int idx = rs232_port_idx(dev);

	if (idx >= 0) {
		uart_irq_rx_disable(dev);
		rs232_irq_rx_ring_reset(idx);
		rs232_hw_poll_drain(dev);
		uart_irq_rx_enable(dev);
		return;
	}
#endif
	rs232_hw_poll_drain(dev);
}

/** FIFO에 있던 바이트를 한 번에 비우고, 읽은 게 있으면 idle_ms 무음까지 대기(최대 cap_ms). 빈 라인은 즉시 반환. */
static void rs232_prepare_tx_bus(const struct device *dev, uint32_t idle_ms, uint32_t cap_ms)
{
	uint8_t ch;
	int had = 0;

#if defined(CONFIG_UART_INTERRUPT_DRIVEN) && CONFIG_UART_INTERRUPT_DRIVEN
	int idx = rs232_port_idx(dev);

	if (idx >= 0) {
		uart_irq_rx_disable(dev);
		while (rs232_irq_rx_ring_pop(idx, &ch) == 0) {
			had = 1;
			(void)ch;
		}
		while (uart_poll_in(dev, &ch) == 0) {
			(void)ch;
			had = 1;
		}
		uart_irq_rx_enable(dev);
	} else
#endif
	{
		while (uart_poll_in(dev, &ch) == 0) {
			(void)ch;
			had = 1;
		}
	}
	if (had != 0) {
		rs232_discard_until_rx_idle(dev, idle_ms, cap_ms);
	}
}

uint8_t rs232_get_protocol(uint8_t port_id)
{
	if (port_id >= RS232_MAX_PORTS) {
		return 0x00U;
	}
	return rs232_protocol[port_id];
}

bool rs232_is_ready(uint8_t port_id)
{
	if (port_id >= RS232_MAX_PORTS) {
		return false;
	}
	const struct device *d = dev_by_port[port_id];

	return d != NULL && device_is_ready(d);
}

static void rs232_mb_hex_line(const char *tag, const uint8_t *d, size_t len)
{
	printf("[RS232] %s (%u B):", tag, (unsigned)len);
	for (size_t i = 0; i < len; i++) {
		printf(" %02x", d[i]);
	}
	printf("\n");
}

/**
 * Modbus RTU 응답 예상 총 바이트 (마지막 2바이트가 CRC16). 파싱 불가 시 -1.
 * FC 01/02/03/04 정상: 1+1+1+bc(데이터)+2(CRC) = 5+bc. 예외 응답: 5. FC 05/06/16: 8.
 */
static int rs232_modbus_expected_resp_len(const uint8_t *buf, size_t n)
{
	if (n < 2U) {
		return -1;
	}

	uint8_t fc = buf[1];

	if ((fc & 0x80U) != 0U) {
		return 5;
	}

	switch (fc) {
	case 0x01:
	case 0x02:
	case 0x03:
	case 0x04:
		if (n < 3U) {
			return -1;
		}
		{
			unsigned bc = buf[2];

			if (bc > 250U) {
				return -1;
			}
			return (int)(5U + bc);
		}
	case 0x05:
	case 0x06:
	case 0x10:
		return 8;
	default:
		return -1;
	}
}

/* ════════════════════════════════════════════════════════════════
 * Modbus ASCII 지원
 * ════════════════════════════════════════════════════════════════
 *
 * 프레임 형식: ':' + HEX(Addr+FC+Data) + HEX(LRC) + '\r\n'
 * LRC = 2의 보수(Addr+FC+Data 바이트 합)
 *
 * TCP→UART: RTU ADU(CRC16 포함) 수신 → CRC16 제거 → ASCII 인코딩 → UART 송신
 * UART→TCP: ASCII 응답 수신 → LRC 검증 → 디코딩 → CRC16 재계산 → RTU ADU 반환
 */

/* ASCII 프레임 최대 크기: ':' + 256*2(hex) + 2(LRC hex) + 2(CRLF) */
#define RS232_ASCII_FRAME_MAX 520U

/* nibble(0..15) → 대문자 ASCII hex 문자 */
static uint8_t rs232_to_hex_char(uint8_t n)
{
	return (n < 10U) ? (uint8_t)('0' + n) : (uint8_t)('A' + n - 10U);
}

/* ASCII hex 문자 → nibble(0..15). 유효하지 않으면 -1 */
static int rs232_hex_nibble(uint8_t c)
{
	if (c >= '0' && c <= '9') {
		return (int)(c - '0');
	}
	if (c >= 'A' && c <= 'F') {
		return 10 + (int)(c - 'A');
	}
	if (c >= 'a' && c <= 'f') {
		return 10 + (int)(c - 'a');
	}
	return -1;
}

/* LRC: 전달 바이트 합의 2의 보수 */
static uint8_t rs232_lrc(const uint8_t *data, size_t len)
{
	uint8_t sum = 0U;

	for (size_t i = 0; i < len; i++) {
		sum += data[i];
	}
	return (uint8_t)(~sum + 1U);
}

/* CRC16-Modbus (poly=0xA001, init=0xFFFF) */
static uint16_t rs232_crc16(const uint8_t *data, size_t len)
{
	uint16_t crc = 0xFFFFU;

	for (size_t i = 0; i < len; i++) {
		crc ^= (uint16_t)data[i];
		for (int b = 0; b < 8; b++) {
			if ((crc & 1U) != 0U) {
				crc = (crc >> 1) ^ 0xA001U;
			} else {
				crc >>= 1;
			}
		}
	}
	return crc;
}

/**
 * RTU ADU(addr+FC+data+CRC16) → Modbus ASCII 프레임 (':' HEX LRC '\r\n').
 * CRC16 마지막 2바이트를 제거하고 LRC 계산·ASCII 인코딩.
 * @return 출력 바이트 수, 실패 시 -1.
 */
static int rs232_rtu_to_ascii_frame(const uint8_t *rtu, size_t rtu_len,
				    uint8_t *out, size_t out_cap)
{
	if (rtu_len < 4U) { /* 최소: addr+FC+CRC16 */
		return -1;
	}
	const size_t pdu_len = rtu_len - 2U; /* CRC16 제거 */
	const size_t need    = 1U + pdu_len * 2U + 4U; /* ':' + hex + LRC(2) + CRLF(2) */

	if (out_cap < need) {
		return -1;
	}

	size_t pos = 0;

	out[pos++] = ':';
	for (size_t i = 0; i < pdu_len; i++) {
		out[pos++] = rs232_to_hex_char((rtu[i] >> 4) & 0x0FU);
		out[pos++] = rs232_to_hex_char(rtu[i] & 0x0FU);
	}
	uint8_t lrc = rs232_lrc(rtu, pdu_len);

	out[pos++] = rs232_to_hex_char((lrc >> 4) & 0x0FU);
	out[pos++] = rs232_to_hex_char(lrc & 0x0FU);
	out[pos++] = '\r';
	out[pos++] = '\n';
	return (int)pos;
}

/**
 * Modbus ASCII 수신 프레임(':' HEX LRC '\r\n') 디코딩.
 * LRC 검증 후 PDU에 CRC16 재계산·추가 → RTU ADU 반환.
 * @return 0: 성공, -1: LRC 불일치, -2: 형식 오류
 */
static int rs232_ascii_decode_response(const uint8_t *ascii, size_t ascii_len,
				       uint8_t *rtu_out, size_t rtu_cap,
				       size_t *rtu_len_out)
{
	/* 최소 10B: ':' + 2(addr) + 2(FC) + 2(LRC) + '\r\n' */
	if (ascii_len < 10U || ascii[0] != ':' ||
	    ascii[ascii_len - 2U] != '\r' || ascii[ascii_len - 1U] != '\n') {
		return -2;
	}

	const size_t hex_len = ascii_len - 3U; /* ':' + '\r\n' 제외 */

	if ((hex_len & 1U) != 0U || hex_len < 6U) {
		return -2;
	}

	const size_t bin_len = hex_len / 2U; /* addr+FC+data+LRC */

	if (bin_len > 256U) {
		return -2;
	}

	uint8_t bin[256];
	const uint8_t *hex = ascii + 1U;

	for (size_t i = 0; i < bin_len; i++) {
		int hi = rs232_hex_nibble(hex[i * 2U]);
		int lo = rs232_hex_nibble(hex[i * 2U + 1U]);

		if (hi < 0 || lo < 0) {
			return -2;
		}
		bin[i] = (uint8_t)(((unsigned)hi << 4) | (unsigned)lo);
	}

	/* LRC 검증: bin[0..bin_len-2] 합의 2의 보수 == bin[bin_len-1] */
	uint8_t lrc_calc = rs232_lrc(bin, bin_len - 1U);

	if (lrc_calc != bin[bin_len - 1U]) {
		printf("[RS232] ASCII LRC mismatch: calc=0x%02x rx=0x%02x\n",
		       lrc_calc, bin[bin_len - 1U]);
		return -1;
	}

	/* PDU(LRC 제외) + CRC16 → RTU ADU */
	const size_t pdu_len = bin_len - 1U;
	const size_t rtu_len = pdu_len + 2U;

	if (rtu_cap < rtu_len) {
		return -2;
	}

	memcpy(rtu_out, bin, pdu_len);
	uint16_t crc             = rs232_crc16(bin, pdu_len);
	rtu_out[pdu_len]         = (uint8_t)(crc & 0xFFU);
	rtu_out[pdu_len + 1U]    = (uint8_t)(crc >> 8);
	*rtu_len_out             = rtu_len;
	return 0;
}

/**
 * Modbus ASCII TX/RX.
 * req: RTU ADU(CRC16 포함). UART로는 ASCII 프레임 송신,
 * 수신 ASCII 프레임을 디코딩 후 RTU ADU(CRC16 재계산) 반환.
 */
static int rs232_modbus_ascii_txrx(uint8_t port_id, const uint8_t *req, size_t req_len,
				   uint8_t *resp, size_t resp_cap, size_t *resp_len_out)
{
	const struct device *dev = dev_by_port[port_id];

	if (dev == NULL || !device_is_ready(dev)) {
		return -ENODEV;
	}

	/*
	 * 패스스루 감지: 첫 바이트 == ':' (0x3A)  AND  끝 2바이트 == '\r\n'
	 * → 클라이언트가 이미 Modbus ASCII 프레임을 완성해서 보낸 경우.
	 * 그 외 → RTU ADU로 보고 ASCII 인코딩 후 전송 (기존 동작).
	 */
	const bool passthrough = (req_len >= 3U &&
				  req[0] == ':' &&
				  req[req_len - 2U] == '\r' &&
				  req[req_len - 1U] == '\n');

	static uint8_t ascii_tx[RS232_ASCII_FRAME_MAX];
	size_t tx_len;

	if (passthrough) {
		/* ASCII 프레임 그대로 복사 */
		if (req_len > sizeof(ascii_tx)) {
			printf("[RS232] ASCII passthrough: frame too large (%u B)\n",
			       (unsigned)req_len);
			return -EINVAL;
		}
		memcpy(ascii_tx, req, req_len);
		tx_len = req_len;
		rs232_mb_hex_line("ASCII TX (passthrough)", ascii_tx, tx_len);
	} else {
		/* RTU ADU → ASCII 프레임 인코딩 */
		int ret = rs232_rtu_to_ascii_frame(req, req_len, ascii_tx, sizeof(ascii_tx));

		if (ret < 0) {
			printf("[RS232] ASCII encode fail (req_len=%u)\n", (unsigned)req_len);
			return -EINVAL;
		}
		tx_len = (size_t)ret;
		rs232_mb_hex_line("ASCII TX (RTU->ASCII)", ascii_tx, tx_len);
	}

	rs232_prepare_tx_bus(dev, 100U, 800U);

	/* TX 동안 RX IRQ 비활성화 (에코·잡음 방지) */
#if defined(CONFIG_UART_INTERRUPT_DRIVEN) && CONFIG_UART_INTERRUPT_DRIVEN
	{
		int irq_idx = rs232_port_idx(dev);

		if (irq_idx >= 0) {
			uart_irq_rx_disable(dev);
			rs232_irq_rx_ring_reset(irq_idx);
			rs232_hw_poll_drain(dev);
		}
	}
#endif

	if (rs232_send(port_id, ascii_tx, tx_len) != 0) {
		printf("[RS232] ASCII UART TX fail\n");
		rs232_drain_rx_fifo(dev);
		return -EIO;
	}

#if defined(CONFIG_UART_INTERRUPT_DRIVEN) && CONFIG_UART_INTERRUPT_DRIVEN
	{
		int irq_idx = rs232_port_idx(dev);

		if (irq_idx >= 0) {
			rs232_hw_poll_drain(dev);
			uart_irq_rx_enable(dev);
		}
	}
#endif

	if (CONFIG_SMARTGATEWAY_RS232_MODBUS_POST_TX_DELAY_MS > 0) {
		k_msleep(CONFIG_SMARTGATEWAY_RS232_MODBUS_POST_TX_DELAY_MS);
	}

	/* ASCII 수신: ':' 대기 → '\r\n' 까지 수집 */
	static uint8_t ascii_rx[RS232_ASCII_FRAME_MAX];
	size_t ascii_n = 0;
	const int64_t t0       = k_uptime_get();
	const int64_t deadline = t0 + RS232_MODBUS_RX_TMO_MS;

	rs232_uart_clear_errors(dev, true);

	/* ':' 시작 문자 대기 */
	while (k_uptime_get() < deadline) {
		if (rs232_abort_flag) {
			rs232_drain_rx_fifo(dev);
			return -ECANCELED;
		}
		uint8_t ch;

		if (rs232_rx_try_byte(dev, &ch) == 0) {
			if (ch == ':') {
				ascii_rx[ascii_n++] = ch;
				break;
			}
		} else {
			k_msleep(1);
		}
	}

	if (ascii_n == 0) {
		printf("[RS232] ASCII RX: ':' timeout after %lld ms\n",
		       (long long)(k_uptime_get() - t0));
		rs232_drain_rx_fifo(dev);
		*resp_len_out = 0;
		return -ETIMEDOUT;
	}

	/* ':' 이후 '\r\n' 까지 수집 */
	bool got_crlf = false;

	while (k_uptime_get() < deadline && ascii_n < sizeof(ascii_rx)) {
		if (rs232_abort_flag) {
			rs232_drain_rx_fifo(dev);
			*resp_len_out = 0;
			return -ECANCELED;
		}
		uint8_t ch;

		if (rs232_rx_try_byte(dev, &ch) == 0) {
			ascii_rx[ascii_n++] = ch;
			if (ascii_n >= 2U &&
			    ascii_rx[ascii_n - 2U] == '\r' &&
			    ascii_rx[ascii_n - 1U] == '\n') {
				got_crlf = true;
				break;
			}
		} else {
			k_msleep(1);
		}
	}

	rs232_mb_hex_line("ASCII RX", ascii_rx, ascii_n);

	if (!got_crlf) {
		printf("[RS232] ASCII RX: CRLF timeout (%u B collected)\n", (unsigned)ascii_n);
		rs232_drain_rx_fifo(dev);
		*resp_len_out = 0;
		return -ETIMEDOUT;
	}

	if (passthrough) {
		/*
		 * 패스스루 RX: UART에서 받은 ASCII 바이트(:....\r\n)를 그대로 클라이언트로 반환.
		 * 클라이언트가 직접 LRC 검증·디코딩을 담당.
		 */
		if (ascii_n > resp_cap) {
			printf("[RS232] ASCII passthrough RX: resp_cap too small (%u > %u)\n",
			       (unsigned)ascii_n, (unsigned)resp_cap);
			rs232_drain_rx_fifo(dev);
			*resp_len_out = 0;
			return -ENOBUFS;
		}
		memcpy(resp, ascii_rx, ascii_n);
		rs232_mb_hex_line("ASCII RX (passthrough->client)", resp, ascii_n);
		*resp_len_out = ascii_n;
		return 0;
	}

	/* RTU 모드 RX: ASCII 응답 디코딩 → RTU ADU (LRC 검증 + CRC16 재계산) */
	size_t rtu_len = 0;
	int dec = rs232_ascii_decode_response(ascii_rx, ascii_n, resp, resp_cap, &rtu_len);

	if (dec != 0) {
		printf("[RS232] ASCII decode fail: %d\n", dec);
		rs232_drain_rx_fifo(dev);
		*resp_len_out = 0;
		return -EIO;
	}

	rs232_mb_hex_line("ASCII->RTU", resp, rtu_len);
	*resp_len_out = rtu_len;
	return 0;
}


int rs232_modbus_txrx(uint8_t port_id, const uint8_t *req, size_t req_len, uint8_t *resp,
		      size_t resp_cap, size_t *resp_len_out)
{
	if (!req || !resp || !resp_len_out || req_len == 0) {
		return -EINVAL;
	}
	if (port_id >= RS232_MAX_PORTS) {
		return -EINVAL;
	}

	/* 프로토콜 디스패치 */
	if (rs232_protocol[port_id] == 0x00U) {
		/* RS-232 미사용 모드 — Modbus 요청 처리 불가 */
		printf("[RS232] port %u: 프로토콜 미설정(0x00) — txrx 거부\n", port_id);
		return -ENOTSUP;
	}
	if (rs232_protocol[port_id] == 0x02U) {
		return rs232_modbus_ascii_txrx(port_id, req, req_len, resp, resp_cap, resp_len_out);
	}
	/* 0x01: Modbus RTU (이하 기존 코드) */

	const struct device *dev = dev_by_port[port_id];

	if (dev == NULL || !device_is_ready(dev)) {
		return -ENODEV;
	}

	/*
	 * TX 전: 잔여 RX가 있을 때만 무음 idle_ms까지 버림(INTER_FRAME_GAP은 수신 프레임 끝 판별용이라
	 * 그대로 쓰면 공선에서 매 TX마다 장시간 대기). 빈 FIFO면 바로 송신.
	 * 직전 트랜잭션에서 -EIO/타임아웃으로 남은 바이트가 있으면 prepare 첫 루프가 정리한다.
	 */
	rs232_prepare_tx_bus(dev, 100U, 800U);

	rs232_mb_hex_line("UART TX", req, req_len);

	/*
	 * TX 동안 RX IRQ가 켜져 있으면 RS-485 되먹임·에코가 링/HW에 들어와
	 * 다음 응답 앞부분과 붙어 8/11·CRC 누락처럼 보일 수 있음 → 송신 구간만 RX 끄고 비움.
	 */
#if defined(CONFIG_UART_INTERRUPT_DRIVEN) && CONFIG_UART_INTERRUPT_DRIVEN
	{
		int irq_idx = rs232_port_idx(dev);

		if (irq_idx >= 0) {
			uart_irq_rx_disable(dev);
			rs232_irq_rx_ring_reset(irq_idx);
			rs232_hw_poll_drain(dev);
		}
	}
#endif

	if (rs232_send(port_id, req, req_len) != 0) {
		printf("[RS232] UART TX fail\n");
		rs232_drain_rx_fifo(dev);
		return -EIO;
	}

#if defined(CONFIG_UART_INTERRUPT_DRIVEN) && CONFIG_UART_INTERRUPT_DRIVEN
	{
		int irq_idx = rs232_port_idx(dev);

		if (irq_idx >= 0) {
			/* TX 중 IRQ 꺼진 동안 RDRF에 들어온 에코·잡음만 비운 뒤 수신 재개 */
			rs232_hw_poll_drain(dev);
			uart_irq_rx_enable(dev);
		}
	}
#endif

	if (CONFIG_SMARTGATEWAY_RS232_MODBUS_POST_TX_DELAY_MS > 0) {
		k_msleep(CONFIG_SMARTGATEWAY_RS232_MODBUS_POST_TX_DELAY_MS);
	}

	size_t max_rx = resp_cap;

	if (max_rx > RS232_MODBUS_RX_MAX) {
		max_rx = RS232_MODBUS_RX_MAX;
	}

	/*
	 * 수신:
	 *  A) 첫 바이트: ms 단위 대기 + 주기적 err 클리어.
	 *  B) 나머지: 링/FIFO에서 가능한 만큼 즉시 읽고, 부족하면 k_msleep(1)로 양보.
	 *     (k_busy_wait 연속 사용은 CPU 독점으로 네트워크·다른 스레드가 멈춘 것처럼 보일 수 있음.)
	 *     IRQ+링 사용 시 HW FIFO는 ISR이 비워 overrun 완화.
	 */
	size_t n = 0;
	const int64_t t0 = k_uptime_get();
	const int64_t deadline = t0 + RS232_MODBUS_RX_TMO_MS;
	int64_t next_err_poll = t0 + 3000;
	int expected_incl_crc = -1;
	int64_t t_first_rx = 0;

	rs232_uart_clear_errors(dev, true);

	while (k_uptime_get() < deadline && n == 0) {
		if (rs232_abort_flag) {
			rs232_drain_rx_fifo(dev);
			return -ECANCELED;
		}
		uint8_t ch;

		if (rs232_rx_try_byte(dev, &ch) == 0) {
			resp[n++] = ch;
			t_first_rx = k_uptime_get();
			break;
		}
		k_msleep(1);
		if (k_uptime_get() >= next_err_poll) {
			next_err_poll = k_uptime_get() + 3000;
			rs232_uart_clear_errors(dev, true);
		}
	}

	while (k_uptime_get() < deadline && n < max_rx) {
		uint8_t ch;

		while (rs232_rx_try_byte(dev, &ch) == 0 && n < max_rx) {
			resp[n++] = ch;
			if (n >= 2U) {
				expected_incl_crc = rs232_modbus_expected_resp_len(resp, n);
			}
			if (expected_incl_crc > 0 && (int)n >= expected_incl_crc) {
				goto rx_done;
			}
		}

		expected_incl_crc = rs232_modbus_expected_resp_len(resp, n);
		if (expected_incl_crc > 0 && (int)n >= expected_incl_crc) {
			break;
		}

		/*
		 * 예상 길이를 알고 나머지를 받는 동안: 115200에서 바이트 간격 ~87µs라
		 * k_msleep(1)만 쓰면 HW FIFO가 차서 OVERRUN 날 수 있음.
		 * 첫 RX 후 짧은 윈도(25ms) 안에서는 짧은 busy-wait로 ISR/링을 따라잡고,
		 * 그 밖(장시간 무응답)에서는 ms sleep으로 CPU 양보.
		 */
		if (rs232_abort_flag) {
			rs232_drain_rx_fifo(dev);
			*resp_len_out = n;
			return -ECANCELED;
		}
		if (expected_incl_crc > 0 && (int)n < expected_incl_crc && n > 0 && t_first_rx > 0 &&
		    (k_uptime_get() - t_first_rx) < 25) {
			k_busy_wait(50);
		} else {
			k_msleep(1);
		}
	}

rx_done:
	*resp_len_out = n;

	/* 항상 수신 결과 덤프: 미수신(0B)·불완전·정상 모두 동일 태그로 확인 가능 */
	if (n == 0) {
		rs232_mb_hex_line("UART RX", resp, 0);
		printf("[RS232] RX timeout after %lld ms (no bytes)\n",
		       (long long)(k_uptime_get() - t0));
		rs232_drain_rx_fifo(dev);
		return -ETIMEDOUT;
	}

	rs232_mb_hex_line("UART RX", resp, n);

	if (expected_incl_crc > 0 && (int)n < expected_incl_crc) {
		printf("[RS232] RX incomplete %u/%d B (expected incl. CRC) → -EIO\n", (unsigned)n,
		       expected_incl_crc);
		rs232_drain_rx_fifo(dev);
		return -EIO;
	}
	/* 예외 응답 최소 5B; 정상 FC는 보통 5B 이상. 그보다 짧으면 불완전으로 본다 */
	if (expected_incl_crc <= 0 && n < 5) {
		printf("[RS232] RX too short for Modbus RTU (%u B, len unknown) → -EIO\n", (unsigned)n);
		rs232_drain_rx_fifo(dev);
		return -EIO;
	}

	/* 성공 직후 FIFO 찌꺼기만 제거(있을 때만 짧은 무음 대기) */
	{
		uint8_t ch;
		int tr = 0;

		while (rs232_rx_try_byte(dev, &ch) == 0) {
			(void)ch;
			tr = 1;
		}
		if (tr != 0) {
			rs232_discard_until_rx_idle(dev, 40U, 120U);
		}
	}

	return 0;
}

int rs232_task_start(void)
{
	rs232_ports_init();
	rs232_print_uart_kconfig();

#if defined(CONFIG_SMARTGATEWAY_DEBUG_CONSOLE)
	printf("[RS232] 테스트 모드: UART0(P1_8/P1_9) = 콘솔(COM6), RS-232 포트 0 비활성\n");
#endif

	static const char *const port_label[RS232_MAX_PORTS] = {
		"UART0 flexcomm4 P1_8/P1_9",
		"UART1 flexcomm5 P1_16/P1_17",
	};
	bool any_ok = false;

	for (int i = 0; i < RS232_MAX_PORTS; i++) {
		const struct device *u = dev_by_port[i];

		if (u == NULL || !device_is_ready(u)) {
			printf("[RS232] port %d (%s) not ready\n", i, port_label[i]);
			continue;
		}
		if (rs232_uart_setup(u) != 0) {
			printf("[RS232] port %d uart_configure failed\n", i);
			continue;
		}
#if defined(CONFIG_UART_INTERRUPT_DRIVEN) && CONFIG_UART_INTERRUPT_DRIVEN
		{
			int e = uart_irq_callback_user_data_set(u, rs232_uart_irq_isr, NULL);

			if (e != 0) {
				printf("[RS232] port %d irq_callback_set failed: %d\n", i, e);
				continue;
			}
			uart_irq_rx_enable(u);
			printf("[RS232] port %d (%s): IRQ RX ready (ring %u B)\n",
			       i, port_label[i], RS232_IRQ_RX_RING_SZ);
		}
#endif
		any_ok = true;
	}

	return any_ok ? 0 : -1;
}
