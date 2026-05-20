/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SYNC_GATE_H
#define SYNC_GATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void sg_timesync_from_tcp_notify(const uint8_t *body, size_t body_len);

/* TCP MsgType 0x01(SYNC) 시각 동기 처리 후 true — UDP TX/RX 공통 게이트 */
bool sg_udp_allowed(void);

#endif
