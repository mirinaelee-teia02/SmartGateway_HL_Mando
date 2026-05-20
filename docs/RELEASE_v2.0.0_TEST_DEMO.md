# Smart Gateway — TEST DEMO v2.0.0

**릴리스 일자**: 2026-05  
**이전 Git 기준**: `a728b58` (Update gateway protocol and WiFi configuration) / 초기 `f8cf463` (V2.7.1)

---

## 요약

프로토콜 문서에 맞춘 TCP/UDP/RS-232 게이트웨이 재정립, WiFi·이더넷 선택 운용, UART(RS-232) 원격 설정, 공통 오류 코드(0x8E/UDP ERROR), ADC 2채널(J4 ADC0_A0/B0) 및 RS-232 동시 동작 안정화를 포함한 **TEST DEMO** 빌드입니다.

CONNECT 프레임 버전: **2.0.0** (`CONFIG_SMARTGATEWAY_TCP_CONNECT_VERSION_HEX=020000`)

---

## 1. WiFi / 이더넷 사용

| 항목 | 내용 |
|------|------|
| 네트워크 | `network_manager` — Kconfig/NVS로 **WiFi** 또는 **이더넷** 선택, TCP/UDP 피어는 NVS·기본값 병용 |
| WiFi | NXP IW416 SDIO, `wifi_manager` 연동 |
| 이더넷 | 보드 기본 ENET 사용 가능 |
| UDP | `WIFI_ENABLE`에 따라 `SMARTGATEWAY_WIFI_*` / `SMARTGATEWAY_ETH_*` 피어·바인드 포트 자동 선택 |
| 게이트 | TCP **0x01 SYNC** 수신 전까지 UDP TX/RX 대기 (`sync_gate`) |

---

## 2. UART(RS-232) 통신 설정

| 항목 | 내용 |
|------|------|
| 포트 | UART1 (FlexComm5, MikroBUS J5) — Modbus 게이트웨이 데이터 포트 |
| 원격 설정 | 서버 **0x01 SYNC** Body 끝 5바이트(BPS, Data, Stop, Parity, Flow) → `rs232_reconfigure()` |
| 프로토콜 | Modbus RTU(0x01) / ASCII(0x02), IRQ RX 링 버퍼 |
| Modbus | TCP **0x02** → RS-232 TX/RX → **0x82** (성공 시만); CRC 검증 없이 패스스루 정렬 |
| 무응답 | **5회** 재시도 후 **0x02** 알람(0x8E), **0x82 미전송** |
| 부팅 메뉴 | NVS 영문 리스트 메뉴 (`config_nvs.c`), UART 콘솔 |

---

## 3. 프로토콜 문서 기준 재정립

| 구분 | 변경 |
|------|------|
| TCP 핸드셰이크 | 0x80 CONNECT → 0x01 SYNC → 0x81 ACK (compact) |
| TCP 세션 | 0x02 Modbus 요청 / 0x82 응답 (Len+STX, 시각 9B + RTU) |
| TCP 알람 | **0x8E** compact: `55 8E Seq [err] 03` (0x01·0x02 등) |
| UDP | MessagePack 28필드, `ERROR_Code` = 공통 `gw_error` |
| 시각 | SYNC Body 9B → `time_helper` UTC 동기 |
| ADC UDP | 2ms 샘플, 20ms 전송, `Sampling_Count` 주기 리셋 |

---

## 4. 이전 Git 대비 주요 변경 사항

### 4.1 신규 / 구조

- **`gw_error.c` / `gw_error.h`**: 오류 코드 0x00~0x05, SYNC Body 검증, TCP 0x8E 송신 API
- **`tcp_gateway.h`**: `gw_tcp_send_alarm()` export
- ADC: 2ms `k_timer` + 세마포어 drain 수정, 스레드 우선순위(ADC 3, UDP 4, TCP GW 6)
- ADC 채널: **CH0A/CH0B** (`ADC0_A0`, `ADC0_B0`) — `input_positive` 0x00 / 0x20

### 4.2 TCP / RS-232

- Modbus RX: `k_busy_wait` → `k_usleep` (IRQ 모드) — ADC 샘플링과 공존
- RS-232 실패 시 **0x8E만** (이전: 0x82 time-only 동시 송신 제거)
- SYNC(0x01) 시각·RS-232 wire 값 범위 검증 → **0x01** 알람
- TCP 끊김/connect 실패 → **0x04** (UDP ERROR_Code, 0x8E는 미송신)

### 4.3 ADC / UDP

- `CONFIG_SMARTGATEWAY_ADC_CHANNEL_COUNT=2`
- MessagePack 운영 포맷(DC 00 1C, 28필드) 정리
- UDP send 실패 → **0x05**

### 4.4 NVS / UI

- 부팅 설정: 라인 위저드 → **영문 리스트 메뉴**
- NVS schema v5 유지

### 4.5 문서

- `docs/TCP_MODBUS_GATEWAY.md`, `docs/ADC_MINMAX_ALGORITHM.md` 갱신
- 본 릴리스 노트 추가

### 4.6 제외 (커밋 안 함)

- `build_netmgr_wifi_ram/`, `debug/` — 빌드 산출물
- `SmartGateway_HL_Mando/` — 로컬 복제본(중첩 `.git`)

### 4.7 이전 커밋 대비 제거·미구현

- `syslog_wifi` 모듈 (이전 커밋에서 이미 제거)
- **0x03** AD7327 SPI 알람: 코드만 정의, 감지 미구현
- **0x04** TCP 알람: UDP ERROR만, TCP 0x8E 미송신

---

## 빌드

```text
west build -b frdm_mcxn947/mcxn947/cpu0 -d debug
west flash -d debug
```

---

## 오류 코드 요약

| Code | 의미 |
|------|------|
| 0x00 | 정상 |
| 0x01 | 0x01 SYNC 시각/RS-232 설정 이상 |
| 0x02 | RS-232 5회 재시도 후 무응답 (0x8E) |
| 0x03 | AD7327 SPI (예약) |
| 0x04 | TCP 통신 불량 (UDP) |
| 0x05 | UDP 통신 불량 |
