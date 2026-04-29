/*
 * Smart Gateway — RS-232 (UART TTL ↔ 외부 레벨 변환)
 *
 * 포트 0: FRDM-MCXN947 MikroBUS J5 — LPFLEXCOMM5 LPUART5
 *         P1_16 RX, P1_17 TX (boards/frdm_mcxn947_mcxn947_cpu0.overlay).
 * Kconfig: 보율/데이터·패리티·스톱·플로우.
 * TCP Modbus 게이트웨이 미사용 시: UDP(RS232_UDP_LISTEN_PORT) → UART 패스스루.
 * 포트 1: 예약.
 */

#ifndef RS232_H
#define RS232_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RS232_MAX_PORTS  2

/**
 * 클라이언트 0x00 TIMESYNC에서 수신한 RS-232 설정 (6바이트).
 * tcp_gateway.c → rs232_reconfigure() 전달용.
 */
typedef struct {
	uint8_t protocol; /* 0x00=RS-232(미사용), 0x01=Modbus RTU, 0x02=Modbus ASCII */
	uint8_t bps;      /* 0x00=9600, 0x01=115200 */
	uint8_t data_bit; /* 0x00=7bit, 0x01=8bit */
	uint8_t stop_bit; /* 0x00=1bit, 0x01=2bit */
	uint8_t parity;   /* 0x00=None, 0x01=ODD, 0x02=EVEN */
	uint8_t flow;     /* 0x00=None, 0x01=RTS/CTS */
} rs232_remote_cfg_t;

/**
 * RS-232 스레드 시작 (설정된 포트 UART 초기화).
 * TCP Modbus 게이트웨이 사용 시 UDP 패스스루 스레드는 생성하지 않음.
 * @return 0 성공, -1 실패
 */
int rs232_task_start(void);

/**
 * RS-232 송신(TX): UART TX 라인으로 len 바이트를 내보냄 (poll 방식).
 * @param port_id 0 .. RS232_MAX_PORTS-1
 * @return 0 성공, -EINVAL, -ENODEV
 */
int rs232_send(uint8_t port_id, const uint8_t *data, size_t len);

/** @ref rs232_send 와 동일 (TX 전용 이름으로 검색·호출 용이). */
int rs232_tx(uint8_t port_id, const uint8_t *data, size_t len);

/**
 * 현재 포트에 설정된 프로토콜 반환.
 * 0x00=RS-232(미사용), 0x01=Modbus RTU, 0x02=Modbus ASCII.
 * tcp_gateway.c에서 Body 파싱 방식 결정에 사용.
 */
uint8_t rs232_get_protocol(uint8_t port_id);

/**
 * RS-232(UART) 포트가 초기화되어 사용 가능한지.
 * tcp_modbus 게이트웨이에서 연결 전 검사용.
 */
bool rs232_is_ready(uint8_t port_id);

/**
 * Modbus TX/RX — rs232_reconfigure()로 설정된 프로토콜에 따라 자동 디스패치.
 *
 * [RTU, protocol=0x01]
 * - req: RTU ADU (addr+FC+data+CRC16). UART로 바이너리 그대로 송신.
 * - resp: 슬레이브 RTU 응답 (CRC16 포함) 그대로 반환.
 * - FC로 예상 길이를 알면 채워질 때까지 poll+busy-wait; 미지원 FC는 타임아웃까지 poll.
 *
 * [ASCII, protocol=0x02]
 * - req: RTU ADU(CRC16 포함) 수신 → CRC16 제거 → ':' HEX LRC '\r\n' ASCII 프레임으로 UART 송신.
 * - resp: ASCII 응답 수신 → LRC 검증 → 디코딩 → CRC16 재계산 → RTU ADU 반환.
 *
 * @param resp_len_out 실제 수신 길이
 * @return 0, -EINVAL, -ENODEV, -ENOTSUP(protocol=0x00), -ETIMEDOUT, -EIO, -ECANCELED
 */
int rs232_modbus_txrx(uint8_t port_id, const uint8_t *req, size_t req_len, uint8_t *resp,
		      size_t resp_cap, size_t *resp_len_out);

/**
 * 진행 중인 rs232_modbus_txrx() 수신 대기를 즉시 중단시킴.
 * TCP 탈취 등 외부에서 RS-232 루프를 강제 종료할 때 사용.
 * 다음 txrx 호출 전 rs232_abort_clear()로 플래그를 리셋해야 함.
 */
void rs232_abort_txrx(void);

/** rs232_abort_txrx()로 설정된 중단 플래그 초기화 (다음 정상 txrx 전 호출). */
void rs232_abort_clear(void);

/**
 * 클라이언트 설정값으로 UART 재구성. 최대 3회 시도 (실패 시 2초 대기).
 * 중간에 rs232_abort_txrx()가 호출되면 즉시 중단.
 * @return 0 성공(OK), -1 전체 실패(NG)
 */
int rs232_reconfigure(uint8_t port_id, const rs232_remote_cfg_t *cfg);

#endif /* RS232_H */
