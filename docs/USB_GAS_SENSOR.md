# SmartGateway USB 가스 센서 구현 문서

**버전:** V2.9.0  
**MCU:** NXP MCXN947 (Cortex-M33 Dual-Core)  
**작성일:** 2026-06-16

---

## 1. 개요

SmartGateway는 USB HOST 기능을 이용하여 FT2232 USB-UART 브릿지를 통해 최대 2개의 가스 센서를 동시에 운용한다.

```
MCXN947
├── USB1 (EHCI HS) ──── FT2232 ──── 센서 (O2/H2S/CO/CH4/CO2/S300)
└── USB0 (KHCI FS) ──── FT2232 ──── 센서 (O2/H2S/CO/CH4/CO2/S300)
```

센서 모델은 NVS에 포트별로 저장되어 전원을 꺼도 유지된다.  
콘솔 메뉴(T키)에서 런타임 변경 가능하다.

---

## 2. 하드웨어 구성

### 2.1 USB 컨트롤러

| 포트 | 컨트롤러 | 속도 | DT 노드 | 커넥터 |
|------|----------|------|---------|--------|
| USB1 | EHCI (High-Speed) | HS 480Mbps | `usb1` | CON201 포트1 |
| USB0 | KHCI (Full-Speed) | FS 12Mbps  | `usb0` | CON201 포트2 |

### 2.2 USB1 (EHCI) 오버레이

```dts
&usb1 {
    compatible = "nxp,uhc-ehci";
    phy-handle = <&usbphy1>;
    status = "okay";
};

&usbphy1 {
    status = "okay";
    tx-d-cal = <4>;
    tx-cal-45-dp-ohms = <7>;
    tx-cal-45-dm-ohms = <7>;
};
```

### 2.3 USB0 (KHCI) 오버레이

```dts
&usb0 {
    compatible = "nxp,uhc-khci";
    /delete-property/ no-voltage-regulator;
    status = "okay";
};
```

> **주의:** `no-voltage-regulator` 속성을 반드시 삭제해야 한다.  
> 클럭은 `board.c`에서 `kCLK_48M_to_USB0 → 48MHz FRO_HF`로 공급된다.

### 2.4 FT2232 USB-UART 브릿지

| 항목 | 값 |
|------|-----|
| VID | 0x0403 |
| PID FT2232D | 0x6001 (Full-Speed, 3MHz 기준 클럭) |
| PID FT2232H | 0x6010 (High-Speed, 12MHz 기준 클럭) |
| 사용 채널 | Channel A (Interface A) |
| Bulk IN EP | 0x81 |
| 패킷 크기 | 64 bytes (Full-Speed) |
| 모뎀 상태 헤더 | 패킷 앞 2바이트 (제거 후 콜백) |

---

## 3. 소프트웨어 아키텍처

### 3.1 레이어 구조

```
application (gas_sensor.c)
    │
    ├── gas_sensor_start()
    ├── gas_sensor_get()
    └── gas_sensor_set_model()
            │
ftdi_usb.c (FT2232 드라이버)
    │
    ├── ftdi_usb_init()    ─── ftdi_task (스레드 per 포트)
    ├── ftdi_usb_connected()
    └── [콜백: ftdi_rx_cb_t]
            │
Zephyr USB HOST Stack
    │
    ├── usbh_core.c        ─── usbh_thread (EP 이벤트 처리)
    │                      ─── usbh_bus_thread (연결/해제 이벤트)
    ├── uhc_mcux_khci.c    ─── uhc_mcux_thread (KHCI 태스크)
    └── uhc_mcux_ehci.c    ─── (EHCI, 인터럽트 기반)
            │
MCUX SDK
    ├── usb_host_khci.c    (USB0 KHCI 하드웨어 드라이버)
    └── usb_host_ehci.c    (USB1 EHCI 하드웨어 드라이버)
```

### 3.2 스레드 목록

| 스레드 이름 | 파일 | 우선순위 | 스택 | 역할 |
|-------------|------|----------|------|------|
| `ftdi_usb1` | ftdi_usb.c | PREEMPT(10) | 4096B | USB1 FT2232 폴링/수신 |
| `ftdi_usb0` | ftdi_usb.c | PREEMPT(10) | 4096B | USB0 FT2232 폴링/수신 |
| `usbh` | usbh_core.c | COOP(9) = -10 | 2048B | EP 전송 완료 이벤트 처리 |
| `usbh_bus` | usbh_core.c | COOP(9) = -10 | 2048B | USB 연결/해제/열거 처리 |
| `uhc_mcux_khci` | uhc_mcux_khci.c | COOP(2) = -3 | 4096B | KHCI 하드웨어 태스크 |

> **우선순위 참고 (V2.9.0 전체):** ADC(0) > NET_TX(1) > NET_RX(2) > UDP(3) > NETMGR/WiFi(4) > TCP(5) > DI/DO(7) > ftdi_usb(10).  
> UHC COOP 스레드는 Kconfig로 변경 불가(하드코딩). 선점형 스레드 전체보다 우선 실행되며,  
> USB 열거/전송 구간에서 ADC 태스크가 일시적으로 선점될 수 있다 (측정값 오류는 없음).

### 3.3 이벤트 흐름 (정상 수신)

```
KHCI/EHCI 하드웨어 전송 완료
    → uhc_mcux_transfer_callback()
        → uhc_xfer_return()          [xfer->queued = 0]
            → event_cb() = usbh_event_carrier()
                → k_msgq_put(&usbh_msgq, event, K_NO_WAIT)
                    → [usbh_thread 깨어남]
                        → bulk_rx_callback()
                            → k_sem_give(&ctx->rx_done)
                                → [ftdi_task 깨어남]
                                    → ftdi_rx_cb_t() → gas_sensor parse
```

### 3.4 공유 리소스

| 리소스 | 설명 | USB0/USB1 공유 여부 |
|--------|------|---------------------|
| `usbh_msgq` | EP 완료 이벤트 큐 | 공유 (용량: `USBH_MAX_UHC_MSG`) |
| `usbh_bus_msgq` | 연결/해제 이벤트 큐 | 공유 (용량: `USBH_MAX_UHC_MSG`) |
| `uhc_xfer_pool` | 전송 객체 슬랩 | 공유 (`UHC_XFER_COUNT`) |
| `uhc_buf_pool` | net_buf 풀 | 공유 (`UHC_BUF_COUNT`) |
| `s_usbh_usb1` | USB1 usbh 컨텍스트 | USB1 전용 |
| `s_usbh_usb0` | USB0 usbh 컨텍스트 | USB0 전용 |
| `s_sensors[]` | 센서 데이터/모델 | 포트별 독립 |
| `s_ports[]` | FTDI 포트 컨텍스트 | 포트별 독립 |

---

## 4. Kconfig 설정 (prj.conf)

```kconfig
CONFIG_UHC_NXP_EHCI=y                  # USB1 EHCI 드라이버
CONFIG_UHC_NXP_KHCI=y                  # USB0 KHCI 드라이버
CONFIG_USB_HOST_STACK=y                 # Zephyr USB HOST 스택

CONFIG_USBH_STACK_SIZE=2048             # usbh/usbh_bus 스레드 스택
CONFIG_UHC_NXP_THREAD_STACK_SIZE=4096  # MCUX 드라이버 태스크 스택

CONFIG_UHC_BUF_POOL_SIZE=4096          # net_buf 풀 총 크기 (bytes)
CONFIG_UHC_BUF_COUNT=64                # net_buf 슬랩 개수
CONFIG_UHC_XFER_COUNT=32               # uhc_transfer 슬랩 개수

# USB0+USB1 동시 열거 시 이벤트 큐 포화 방지
# 열거당 ~6개 ctrl xfer 이벤트 + 양쪽 bulk 이벤트 → 기본값 10으로 부족
CONFIG_USBH_MAX_UHC_MSG=32
```

---

## 5. 지원 센서 모델

| 모델 번호 | 열거값 | 센서 이름 | 출력 형식 | 주기 | Baud |
|-----------|--------|-----------|-----------|------|------|
| 0 | `GAS_MODEL_NONE` | 미설정 | — | — | — |
| 1 | `GAS_MODEL_O2_SM30` | O2-SM30-3V | `"XX.XX%\r\n"` | 1s | 38400 |
| 2 | `GAS_MODEL_H2S_SM30` | H2S-SM30-3V | `"F.F ppm\r\n"` | 1s | 38400 |
| 3 | `GAS_MODEL_CO_SM30` | CO-SM30-3V | `"F.F ppm\r\n"` | 1s | 38400 |
| 4 | `GAS_MODEL_CH4_S3_3V` | CH4-S3-3V | `"  DD% LEL\r\n"` (기본) / `"DDDDDD ppm\r\n"` (옵션) | 3s | 38400 |
| 5 | `GAS_MODEL_S300_3V` | S-300-3V (CO2) | `"DDDDDD ppm\r\n"` | 3s | 38400 |
| 6 | `GAS_MODEL_AM1002` | AM1002 (공기질) | 바이너리 22B 프레임 | ~1s | 9600 |

### 5.1 CH4-S3-3V 파서 상세 (`parse_ch4`)

기본 포맷 `"   DD% LEL\r\n"`:
- `% LEL` 문자열 역방향 탐색 → `%` 앞 숫자 추출
- 변환식: **ppm = %LEL × 500** (1% LEL = 500 ppm, CH4 LFL=5% 기준)

옵션 포맷 `"DDDDDD ppm\r\n"`:
- `% LEL` 미감지 시 `parse_ppm()` 재사용

### 5.2 AM1002 바이너리 프레임

```
오프셋  내용
 0      0x16 (프레임 시작 마커)
 1      0x13 (데이터 길이 = 19)
 2      0x16 (고정)
 3-4    TVOC (ppb)
 5-6    예약
 7-8    PM1.0 (μg/m³)
 9-10   PM2.5 (μg/m³)  ← 현재 사용
11-12   PM10  (μg/m³)
13-14   온도 raw (℃ = (raw-500)/10)
15-16   습도 raw (% = raw/10)
17-21   예약
 21     체크섬: (256 - sum(byte[0..20])) & 0xFF
```

현재 구현: PM2.5만 `reading.pct` / `reading.raw`에 저장.

---

## 6. FT2232 UART 설정

### 6.1 baud rate 계산

FTDI SIO_SET_BAUDRATE 요청: `bmRequestType=0x40, bRequest=0x03`

| Baud | 칩종 | wValue | wIndex |
|------|------|--------|--------|
| 38400 | FT2232D (3MHz) | `0x404F` | `0x0000` |
| 38400 | FT2232H (12MHz) | `0x0139` | `0x0002` |

> FT2232H는 wIndex bit1=1로 고속 클럭 모드 지정.

### 6.2 초기화 순서

```
1. SIO_RESET       (bRequest=0x00) — 채널 리셋
2. k_msleep(50)    — 리셋 안정화 대기
3. SIO_SET_BAUD    (bRequest=0x03) — 38400 8N1
4. SIO_SET_DATA    (bRequest=0x04) — 8비트, 패리티 없음, 1 스톱
5. SIO_SET_FLOW    (bRequest=0x02) — 플로우 컨트롤 없음
```

---

## 7. 동시 동작 구조

USB0와 USB1은 **하드웨어 컨트롤러가 독립적**이므로 병렬 데이터 전송이 가능하다.

```
ftdi_task(USB1) ──┐
                  ├── 독립 스레드, 독립 세마포어, 독립 버퍼
ftdi_task(USB0) ──┘

EHCI HW ──────────── USB1 전용 DMA/IRQ
KHCI HW ──────────── USB0 전용 인터럽트

공유: usbh_msgq(32슬롯) ← 양쪽 완료 이벤트가 합산
공유: usbh_thread ← 이벤트를 직렬 처리 (빠름, 우선순위 COOP(9))
```

---

## 8. 알려진 버그 및 수정 내역

### 8.1 USB0 KHCI 열거 실패 — MCUX SDK 버그 수정

**증상:** USB0에 디바이스 연결 시 `Failed to read device descriptor` 반복

**파일:** `modules/hal/nxp/mcux/mcux-sdk-ng/middleware/usb/host/usb_host_khci.c`  
**함수:** `_USB_HostKhciBusControl()` — `kUSB_HostBusReset` 처리

**원인:**
```c
// 수정 전 (버그): USBENSOFEN 비트가 클리어되어 SOF 발생 중단 → 트랜잭션 불가
usbHostPointer->usbRegBase->CTL = USB_CTL_HOSTMODEEN_MASK;

// 수정 후: SOF 재활성화 + USB 2.0 규격 준수 복구 대기 추가
usbHostPointer->usbRegBase->CTL = USB_CTL_HOSTMODEEN_MASK
                                 | USB_CTL_USBENSOFEN_MASK;
_USB_HostKhciDelay(usbHostPointer, 20U);
```

**배경:** 초기 `_USB_HostKhciAttach()`는 올바르게 `CTL |= USBENSOFEN` 처리하나,  
2차 버스 리셋(`kUSB_HostBusReset`)에서 `CTL =` 대입으로 USBENSOFEN을 소거.

---

### 8.2 동시 연결 시 "Transfer is still queued" 오류

**증상:** USB0+USB1 동시 연결 후 `<err> uhc: Transfer is still queued` 출력, 연결 불안정

**원인 1: usbh_msgq 큐 포화**

USB 열거 과정에서 포트당 약 6개의 제어 전송 이벤트가 발생한다.  
USB0, USB1 동시 열거 시 이벤트가 합산되어 기본값 `USBH_MAX_UHC_MSG=10`으로 포화.  
큐 포화 시 `k_msgq_put(..., K_NO_WAIT)` 실패 → 이벤트 드롭 → `k_sem_give` 미실행 → 2초 타임아웃.

**수정:** `prj.conf`에 `CONFIG_USBH_MAX_UHC_MSG=32` 추가.

---

**원인 2: dequeue 후 use-after-free로 인한 슬랩 손상**

타임아웃 발생 시 `uhc_ep_dequeue()` 호출 흐름:

```
uhc_ep_dequeue()
 └─ api->ep_dequeue() = uhc_mcux_dequeue()
     └─ USB_HostKciIoctl(kUSB_HostCancelTransfer)  ← 동기 실행
         └─ uhc_mcux_transfer_callback()
             └─ uhc_xfer_return()                   [xfer->queued = 0]
                 └─ usbh_event_carrier()
                     └─ k_msgq_put(&usbh_msgq, ...)  ← 이벤트 등록
 └─ xfer->queued = 0  (중복 클리어, 무해)
```

취소 콜백이 **동기**로 실행되어 `usbh_msgq`에 이벤트를 적재한 직후,  
기존 코드는 `xfer`를 즉시 해제했다.

이후 다음 `ftdi_bulk_read_once()` 호출에서 **동일한 슬랩 메모리**를 재할당.  
`usbh_thread`가 낡은 이벤트를 처리하면 재사용된 xfer에 `k_sem_give()` 조기 발동.  
→ `ftdi_task`가 아직 `queued=1`인 새 xfer에 대해 `uhc_xfer_free()` 호출 → **"Transfer is still queued"**

```
타임아웃 → uhc_ep_dequeue()
              취소 콜백(동기): usbh_msgq에 old_xfer 이벤트 등록
         → uhc_xfer_free(old_xfer)  ← 슬랩 반환
         ← [다음 iteration]
         → uhc_xfer_alloc()         ← 동일 슬랩 재사용 (new_xfer = old_xfer 주소)
         → uhc_ep_enqueue(new_xfer) ← queued = 1
         ← [usbh_thread가 낡은 이벤트 처리]
         → bulk_rx_callback(old_xfer) ← old_xfer == new_xfer (주소 동일)
         → k_sem_give()              ← 조기 발동!
         ← ftdi_task 깨어남
         → uhc_xfer_free(new_xfer)  ← queued=1 → 오류!
```

**수정:** `uhc_ep_dequeue()` 후 `k_sem_take(200ms)`로 취소 콜백 시그널을 소비한 뒤 해제.

```c
if (k_sem_take(&ctx->rx_done, K_MSEC(2000)) != 0) {
    uhc_ep_dequeue(uhc_dev, xfer);
    /* 취소 콜백이 동기 실행되어 usbh_msgq에 이벤트 등록됨.
     * usbh_thread(높은 우선순위)가 처리 → bulk_rx_callback → k_sem_give.
     * 시그널을 소비한 뒤 해제해야 슬랩 재사용 시 use-after-free 방지. */
    (void)k_sem_take(&ctx->rx_done, K_MSEC(200));
    uhc_xfer_buf_free(uhc_dev, nbuf);
    uhc_xfer_free(uhc_dev, xfer);
    return -ETIMEDOUT;
}
```

---

## 9. NVS 저장 구조

```c
KEY_SENSOR_USB1 = 20  // gas_sensor_model_t (1 byte)
KEY_SENSOR_USB0 = 21  // gas_sensor_model_t (1 byte)
```

T 메뉴(콘솔)에서 모델 변경 시 NVS에 즉시 저장.  
부팅 시 `gas_sensor_start()`에서 NVS 값을 읽어 각 포트에 적용.

---

## 10. API 요약

### ftdi_usb.h

```c
// USB HOST 초기화 및 수신 스레드 시작
int  ftdi_usb_init(ftdi_port_t port, ftdi_rx_cb_t cb, void *user);

// FT2232 연결 여부
bool ftdi_usb_connected(ftdi_port_t port);
```

### gas_sensor.h

```c
// USB HOST + FT2232 두 포트 모두 시작 (NVS에서 모델 읽음)
void gas_sensor_start(void);

// 최근 측정값 반환 (valid=false → 데이터 없음)
gas_reading_t gas_sensor_get(gas_sensor_type_t type);

// FT2232 연결 여부
bool gas_sensor_connected(gas_sensor_type_t type);

// 런타임 모델 변경 (버퍼/측정값 초기화 포함)
void gas_sensor_set_model(gas_port_t port, gas_sensor_model_t model);
gas_sensor_model_t gas_sensor_get_model(gas_port_t port);

// 표시용 이름/단위
const char *gas_model_name(gas_sensor_model_t model);
const char *gas_model_unit(gas_sensor_model_t model);
```

### gas_reading_t 구조체

```c
typedef struct {
    bool     valid;   // false = 아직 데이터 없음
    uint16_t raw;     // O2: %Vol×100 / ppm 센서: ppm / AM1002: PM2.5 μg/m³
    float    pct;     // O2: %Vol / ppm 센서: ppm float / AM1002: PM2.5 float
} gas_reading_t;
```

---

## 11. 부팅 로그 예시 (정상)

```
[00:00:01.234] <inf> ftdi_usb: USB1 HOST ready. Waiting for FT2232...
[00:00:01.235] <inf> ftdi_usb: USB0 HOST ready. Waiting for FT2232...
[00:00:01.890] <inf> ftdi_usb: USB1: FT2232D detected
[00:00:01.990] <inf> ftdi_usb: USB1: FT2232D 38400 8N1 ready
[00:00:02.100] <inf> ftdi_usb: USB0: FT2232D detected
[00:00:02.200] <inf> ftdi_usb: USB0: FT2232D 38400 8N1 ready
```

---

## 12. 트러블슈팅

| 증상 | 원인 | 조치 |
|------|------|------|
| `Failed to read device descriptor` | MCUX SDK KHCI 버그 (USBENSOFEN 클리어) | `usb_host_khci.c` 패치 확인 |
| `Transfer is still queued` | 동시 연결 시 큐 포화 + use-after-free | `USBH_MAX_UHC_MSG=32` + dequeue 후 sem_take(200ms) |
| USB0만 연결 안 됨 | `no-voltage-regulator` 속성 남아있음 | overlay에서 `/delete-property/ no-voltage-regulator` |
| 센서 데이터 0 또는 미수신 | 센서 모델 미설정(NONE) | T 메뉴에서 모델 선택 후 저장 |
| CH4 값이 이상함 | `% LEL` 포맷인데 ppm으로 처리 | `parse_ch4()` 포맷 감지 로직 확인 |
| AM1002 데이터 없음 | 체크섬 오류 또는 9600 baud 미설정 | baud 확인(현재 38400 고정, AM1002=9600 미지원) |
