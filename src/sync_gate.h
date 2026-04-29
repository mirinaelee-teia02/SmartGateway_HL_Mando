/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SYNC_GATE_H
#define SYNC_GATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void sg_timesync_from_tcp_notify(const uint8_t *body, size_t body_len);

bool sg_udp_rx_allowed(void);

#endif
