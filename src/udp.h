/*
 * Smart Gateway - UDP 모듈 헤더
 *
 * 이더넷을 통한 UDP 전송 Task.
 * adc_data.h의 adc_snapshot_t를 주기적으로 전송합니다.
 */

#ifndef UDP_H
#define UDP_H

/* 전송 주기 (ms). 2000 = 2초마다 전송 (조절 가능) */
#define UDP_SEND_INTERVAL_MS 2000

/*
 * udp_task_start
 *
 * UDP 전송 전용 스레드를 생성하고 시작합니다.
 * 네트워크 준비 후, ADC 스냅샷을 주기적으로 target_ip:port로 전송합니다.
 *
 * [반환값]
 *   0: 성공
 *  -1: 스레드 생성 실패
 */
int udp_task_start(void);

#endif /* UDP_H */
