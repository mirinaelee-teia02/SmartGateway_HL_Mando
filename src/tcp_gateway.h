/* SPDX-License-Identifier: Apache-2.0 */
#ifndef TCP_GATEWAY_H
#define TCP_GATEWAY_H

#include <stdint.h>

int tcp_gateway_task_start(void);

/** 연결된 TCP 세션으로 0x8E 알람(compact) 1회 송신. code=GW_ERR_* */
int gw_tcp_send_alarm(int cfd, uint8_t seq, uint8_t code);

#endif
