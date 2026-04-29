# SmartGateway 구현 현황 문서

**버전**: V2.7.1  
**보드**: FRDM-MCXN947 (NXP MCXN947, Cortex-M33)  
**WiFi**: EVK-MAYA-W166 (u-blox IW416, SDIO 연결)  
**작성일**: 2026-04-29

---

## 1. 시스템 구성

```
┌─────────────────────────────────────────────────────┐
│              FRDM-MCXN947                           │
│                                                     │
│  ┌──────────┐   ┌──────────┐   ┌─────────────────┐ │
│  │  LPADC0  │   │  LPUART5 │   │  USDHC0 (SDIO)  │ │
│  │  2ch ADC │   │  RS-232  │   │  IW416 WiFi     │ │
│  └────┬─────┘   └────┬─────┘   └────────┬────────┘ │
│       │              │                  │           │
│  adc_task       rs232_task        wifi_manager     │
│       │              │                  │           │
│  adc_snapshot   RS-232 ↔ Modbus   AP 연결/정적IP  │
│       │              │                  │           │
│  udp_task ──────────────────→ UDP TX (2초 주기)    │
│  tcp_gateway ───── RS-232 ↔ TCP Modbus 중계        │
└─────────────────────────────────────────────────────┘
```

---

## 2. 태스크 구성 및 현재 활성 상태

| 태스크 | 파일 | 우선순위 | 스택 | 현재 상태 |
|--------|------|---------|------|----------|
| wifi_task | wifi_manager.c | 5 | 4096 B | ✅ 활성 |
| adc_task | adc.c | 5 | 1536 B | ✅ 활성 |
| udp_task | udp.c | 4 | 2048 B | ✅ 활성 |
| rs232_task | rs232.c | - | - | ⏸ 주석 처리 |
| tcp_gateway_task | tcp_gateway.c | 5 | - | ⏸ 주석 처리 |

> **부팅 순서**: WiFi 연결 완료(최대 30초 대기) → ADC 시작 → UDP 시작

---

## 3. 네트워크 모드

`prj.conf`에서 한 줄만 `=y`로 설정하여 모드 전환.

| 모드 | 설정 | 자동 동작 |
|------|------|----------|
| 이더넷 전용 | `CONFIG_SMARTGATEWAY_NET_MODE_ETH_ONLY=y` | NET_CONFIG_SETTINGS 활성, WiFi 비활성 |
| **WiFi 전용** | `CONFIG_SMARTGATEWAY_NET_MODE_WIFI_ONLY=y` | WiFi 활성, 이더넷 IP 미설정 |
| 이더넷+WiFi | `CONFIG_SMARTGATEWAY_NET_MODE_BOTH=y` | 둘 다 활성 |

> `CONFIG_NET_IF_MAX_IPV4_COUNT=2` — ETH 하드웨어가 항상 존재하므로 WiFi 포함 시 항상 2 필요.

### UDP 피어 자동 선택 (udp.c)
```c
#if IS_ENABLED(CONFIG_SMARTGATEWAY_WIFI_ENABLE)
    UDP_PEER_IP  = CONFIG_SMARTGATEWAY_WIFI_UDP_PEER_IP
    UDP_PEER_PORT = CONFIG_SMARTGATEWAY_WIFI_UDP_PEER_PORT
    UDP_BIND_PORT = CONFIG_SMARTGATEWAY_WIFI_UDP_BIND_PORT
#else
    UDP_PEER_IP  = CONFIG_SMARTGATEWAY_ETH_UDP_PEER_IP
    ...
#endif
```

---

## 4. WiFi 모듈 (wifi_manager.c)

### 동작 흐름
```
wifi_task_start()
  └─ net_if_get_first_wifi()       WiFi 인터페이스 획득
  └─ net_mgmt_add_event_callback() 연결 이벤트 등록
  └─ k_sleep(2s)                   wpa_supplicant 준비 대기
  └─ net_mgmt(CONNECT, params)     AP 연결 요청
  └─ k_sem_take(&sem_connected)    연결 완료 대기 (30초 타임아웃)
  └─ wifi_set_static_ip()          정적 IP 설정
  └─ net_if_set_default()          WiFi를 기본 인터페이스로 설정
  └─ k_sem_give(&sem_wifi_ready)   준비 완료 신호
```

### WiFi 보안 모드 (Kconfig choice)

| 설정 | security 값 | 용도 |
|------|------------|------|
| `WIFI_SECURITY_WPA2=y` | PSK (1) | WPA2 전용 AP |
| `WIFI_SECURITY_WPA_AUTO=y` | WPA_AUTO_PERSONAL (10) | WPA2/WPA3 자동 (권장) |
| `WIFI_SECURITY_WPA3=y` | SAE_AUTO (5) | WPA3 전용 AP |
| `WIFI_SECURITY_NONE=y` | NONE (0) | 오픈 AP |

> **현재 설정**: `WPA_AUTO` — WPA2/WPA3 AP 모두 자동 대응

---

## 5. ADC 모듈 (adc.c)

### 현재 모드: 내부 LPADC0 2채널 (검증용)

| 항목 | 값 |
|------|-----|
| 드라이버 | `DEVICE_DT_GET(DT_NODELABEL(lpadc0))` |
| 채널 수 | 2 (CH0A: input=0x00, CH0B: input=0x20) |
| 샘플링 주기 | 2 ms |
| 윈도우 | 2,000 ms (1,000 샘플) |
| 분해능 | 12-bit |
| 기준 전압 | 3.3 V (VREF_MV=3300) |
| 전압 범위 | 0 ~ 3.3 V |

### 스냅샷 구조 (adc_snapshot_t)
```c
typedef struct {
    char       line[51];              // LINE_ID (Kconfig)
    datetime_t datetime;              // 전송 시각
    uint16_t   msec;                  // 밀리초
    float      raw[8];                // 최신 샘플 전압 (V)
    float      min_val[8];            // 2초 윈도우 최솟값
    float      max_val[8];            // 2초 윈도우 최댓값
    uint8_t    ch_count;              // 현재 2
} adc_snapshot_t;
```

### 예정 (AD7327 외부 SPI 8채널)
- `adc.c` 하단 `[AD7327 disabled]` 블록 참조
- `CONFIG_SPI=y` + overlay `&flexcomm6_lpspi6` 활성화 시 전환

---

## 6. UDP 모듈 (udp.c)

### 전송 규격
- 주기: **2,000 ms**
- 소켓: `AF_INET / SOCK_DGRAM`
- 페이로드: **MessagePack** 인코딩

### 페이로드 포맷

**테스트 모드** (`CONFIG_SMARTGATEWAY_UDP_TEST_MODE=y`):
```
fixarray(4):
  [0] str  — LINE_ID (예: "KRWJARDU001")
  [1] str  — 타임스탬프 17자 "yyyyMMddHHmmssfff"
  [2] f32  — CH0A 전압 (V, 소수 2자리)
  [3] f32  — CH0B 전압 (V, 소수 2자리)
```

**운영 모드** (`UDP_TEST_MODE=n`):
```
fixmap(5):
  "line"      → str  — LINE_ID
  "timestamp" → str  — 14자 "yyyyMMddHHmmss"
  "raw"       → [f32 × ch_count]  최신 전압
  "min"       → [f32 × ch_count]  윈도우 최솟값
  "max"       → [f32 × ch_count]  윈도우 최댓값
```

### RX (수신)
- `UDP_BIND_PORT > 0` 일 때 활성화
- `poll(timeout=0)` 논블로킹 → 수신 시 앞 64바이트 HEX 로그
- TCP Modbus 게이트웨이 활성 시: `sg_udp_rx_allowed()` = true(시각동기 후)일 때만 수신

---

## 7. TCP Modbus 게이트웨이 (tcp_gateway.c) — 현재 비활성

### 프레임 규격

| 방향 | 구조 |
|------|------|
| 보드→서버 (CONNECT 0x80) | `STX + MsgType + Seq + Body + ETX` |
| 서버→보드 (TIMESYNC 0x00 응답) | `STX + MsgType + Seq + Timestamp(13B) + ETX` |
| 서버→보드 (세션 요청) | `STX + Len(2,BE) + MsgType + Seq + Body + ETX` |
| 보드→서버 (RS-232 결과) | `STX + Len(2,BE) + MsgType + Seq + Body + Error + ETX` |

| 상수 | 값 | 설명 |
|------|-----|------|
| STX | 0x55 | 프레임 시작 |
| ETX | 0x03 | 프레임 종료 |
| CONNECT | 0x80 | 핸드셰이크 (보드→서버) |
| TIMESYNC | 0x00 | 시각동기 (서버→보드) |
| REQUEST | 0x01 | Modbus 요청 (서버→보드) |
| RESPONSE | 0x82 | Modbus 응답 (보드→서버) |
| ERR_TIMEOUT | 0xE3 | RS-232 타임아웃 |

### 탈취 기능 (서버 모드)
- 0x70 CONN_REQ (11바이트 고정 Body) 수신 시 현재 세션 강제 종료
- 새 클라이언트가 세션 획득

### 시각동기 (time_helper.c + sync_gate.c)
- TCP 0x00 Body 앞 7바이트: `[year_h, year_l, month, day, hour, min, sec]`
- 수신 후 `wall_synced=true` → `get_datetime()` 이후 경과 초 반영
- `sg_udp_rx_allowed()` = true → UDP RX 허용

---

## 8. RS-232 모듈 (rs232.c) — 현재 비활성

| 항목 | 포트 0 | 포트 1 |
|------|--------|--------|
| 핀 | P1_8(RX) / P1_9(TX) | P1_16(RX) / P1_17(TX) |
| UART | flexcomm4 / LPUART4 | flexcomm5 / LPUART5 |
| 위치 | MikroBUS J5 | MikroBUS J5 확장 |

- Kconfig로 보율 / 데이터비트 / 패리티 / 스톱비트 / 흐름제어 설정
- TCP Modbus 게이트웨이 비활성 시: UDP 패스스루 모드 동작

---

## 9. 하드웨어 핀 맵

| 기능 | 핀 | UART/IP |
|------|-----|---------|
| 콘솔 (디버그) | P1_8 / P1_9 | LPUART4 (FC4) |
| RS-232 포트 | P1_16 / P1_17 | LPUART5 (FC5) |
| WiFi SDIO | USDHC0 슬롯 | IW416 (microSD 어댑터) |
| ADC CH0A | LPADC0 input=0x00 | 내부 |
| ADC CH0B | LPADC0 input=0x20 | 내부 |

---

## 10. prj.conf 주요 설정 요약

### 현재 활성 값 (WiFi 전용 테스트)

```
네트워크 모드  : WiFi 전용 (NET_MODE_WIFI_ONLY)
AP SSID       : HLMando5G
AP 보안       : WPA_AUTO (WPA2/WPA3 자동)
보드 IP       : 192.168.88.100 / 255.255.255.0  GW: 192.168.88.1
UDP 대상      : 192.168.88.3:20263
UDP 수신 포트 : 20263
UDP 페이로드  : 테스트 모드 (4-element array)
ADC           : LPADC0 2채널 (검증용)
LINE_ID       : KRWJARDU001
```

---

## 11. 향후 계획

| 항목 | 상태 | 비고 |
|------|------|------|
| ADC 검증 (LPADC0 2ch) | ✅ 구현 완료 | 현재 활성 |
| WiFi 연결 (WPA2/WPA3) | ✅ 구현 완료 | WPA_AUTO 사용 |
| UDP ADC 전송 | ✅ 구현 완료 | 2초 주기 MessagePack |
| RS-232 태스크 | 🔲 구현 완료 / 비활성 | main.c 주석 해제로 활성화 |
| TCP Modbus 게이트웨이 | 🔲 구현 완료 / 비활성 | RS-232 활성화 후 함께 활성화 |
| AD7327 외부 SPI ADC (8ch) | 🔲 미검증 | adc.c 하단 블록, CONFIG_SPI=y |
| WPA3 단독 모드 (양산) | 🔲 준비 완료 | `WIFI_SECURITY_WPA3=y`로 전환 |
| 이더넷 전용 / 이더넷+WiFi | 🔲 구성 완료 | NET_MODE 한 줄 변경 |
