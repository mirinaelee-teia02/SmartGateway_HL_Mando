/*
 * Smart Gateway — 공통 오류 코드
 *
 * TCP: compact STX(0x55) + MsgType(0x8E) + Seq + Body(1B code) + ETX(0x03)
 * UDP: MessagePack 필드 28 ERROR_Code — 동일 1바이트.
 */

#ifndef GW_ERROR_H
#define GW_ERROR_H

#include <stddef.h>
#include <stdint.h>

#define GW_ERR_NONE			0x00U	/* 정상 */
#define GW_ERR_INIT_CFG			0x01U	/* 0x01 SYNC 시각·RS-232 설정 이상 */
#define GW_ERR_RS232_NO_RESPONSE	0x02U	/* RS-232 5회 재시도 후 무응답 */
#define GW_ERR_ADC7327_SPI		0x03U	/* AD7327 SPI 불량 (감지 미구현, 코드만) */
#define GW_ERR_TCP_COMM			0x04U	/* TCP 통신 불량 */
#define GW_ERR_UDP_COMM			0x05U	/* UDP 통신 불량 */

#define GW_TIME_WIRE_LEN		9U
#define GW_RS232_CFG_WIRE_LEN		5U
#define GW_SYNC_BODY_LEN		(GW_TIME_WIRE_LEN + GW_RS232_CFG_WIRE_LEN)
#define GW_SYNC_RS232_OFFSET		GW_TIME_WIRE_LEN

#define GW_MSG_ALARM			0x8EU	/* 보드→서버 알람 (Body=오류코드 1B) */

#define GW_RS232_MODBUS_RETRIES		5

void gw_error_set(uint8_t code);
void gw_error_clear(void);
uint8_t gw_error_get(void);

/** RS-232 wire 5바이트(BPS~Flow) 검증. GW_ERR_NONE 또는 GW_ERR_INIT_CFG */
uint8_t gw_rs232_wire_cfg_check(const uint8_t cfg[GW_RS232_CFG_WIRE_LEN]);

/** 0x01 SYNC Body 14바이트 검증. GW_ERR_NONE 또는 GW_ERR_INIT_CFG */
uint8_t gw_sync_body_check(const uint8_t *body, size_t body_len);

#endif /* GW_ERROR_H */
