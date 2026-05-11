/* SPDX-License-Identifier: Apache-2.0 */

#include "sync_gate.h"
#include "time_helper.h"

#include <stdbool.h>
#include <stdio.h>
#include <zephyr/autoconf.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

static volatile bool sg_time_ok;

void sg_timesync_from_tcp_notify(const uint8_t *body, size_t body_len)
{
	time_sync_from_tcp_timesync_body(body, body_len);

	//if (!sg_time_ok) {
	//	printf("[SG] TCP 시각동기(0x00) — UDP 수신 허용\n");
	//}
	sg_time_ok = true;
}

bool sg_udp_allowed(void)
{
	return sg_time_ok;
}
