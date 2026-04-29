# SmartGateway WiFi 개발 기록

**보드:** FRDM-MCXN947  
**WiFi 모듈:** u-blox EVK-MAYA-W166 (NXP IW416, SDIO)  
**OS:** Zephyr RTOS (NXP ZSDK, nxp-v4.3.0)  
**작성일:** 2026-04-28  

---

## 1. 개발 환경 구축

### 1.1 사전 요구사항

| 항목 | 버전 |
|------|------|
| OS | Windows 11 |
| Python | 3.10 이상 |
| CMake | 3.20 이상 |
| Ninja | 최신 |
| Zephyr SDK | 0.16.x |
| west | 1.2 이상 |

### 1.2 west 설치

```powershell
pip install west
```

### 1.3 NXP ZSDK 워크스페이스 초기화

```powershell
# 워크스페이스 폴더 생성
mkdir C:\nxp\Zephyr
cd C:\nxp\Zephyr

# west init — NXP ZSDK manifest 저장소 기준으로 초기화
west init -m https://github.com/nxp-zephyr/zsdk --mr main nxp_zephyr

cd nxp_zephyr

# 모든 서브 저장소 다운로드 (시간 소요)
west update
```

> **결과 폴더 구조:**
> ```
> C:\nxp\Zephyr\nxp_zephyr\
>   ├─ .west/config          ← west 설정 (manifest path = zsdk)
>   ├─ zsdk/                 ← NXP ZSDK manifest (west.yml)
>   ├─ zephyr/               ← Zephyr 커널 (nxp-zephyr, nxp-v4.3.0)
>   ├─ bootloader/
>   ├─ modules/
>   └─ tools/
> ```

### 1.4 NXP WiFi 펌웨어 블롭 다운로드

IW416 WiFi 드라이버는 NXP 독점 펌웨어 바이너리가 필요하다. `west update` 이후 반드시 실행:

```powershell
west blobs fetch hal_nxp
```

> **확인 방법:** `west blobs list hal_nxp` — 상태가 `A`(available)이면 정상

### 1.5 Python 패키지 설치

```powershell
cd C:\nxp\Zephyr\nxp_zephyr
pip install -r zephyr/scripts/requirements.txt
```

### 1.5 Zephyr SDK 설치

[https://github.com/zephyrproject-rtos/sdk-ng/releases](https://github.com/zephyrproject-rtos/sdk-ng/releases) 에서 Windows용 설치파일 다운로드 후 설치.  
기본 경로: `C:\zephyr-sdk-0.16.x`

환경변수 설정 (또는 `CMakePresets.json`에서 지정):

```powershell
$env:ZEPHYR_SDK_INSTALL_DIR = "C:\zephyr-sdk-0.16.x"
```

---

## 2. SmartGateway 프로젝트

### 2.1 위치

```
C:\nxp\Zephyr\nxp_zephyr\zephyr\SmartGateway\
```

Zephyr 커널 트리 안에 직접 위치하며 `west build` 시 경로로 지정.

### 2.2 주요 파일 구조

```
SmartGateway/
├── CMakeLists.txt
├── Kconfig
├── prj.conf                          ← 빌드 설정
├── boards/
│   └── frdm_mcxn947_mcxn947_cpu0.overlay  ← 보드 DTS 확장
├── src/
│   ├── main.c
│   ├── wifi_manager.c / .h           ← WiFi 태스크
│   ├── adc.c / .h                    ← ADC (LPADC0 또는 AD7327)
│   ├── udp.c / .h                    ← UDP 전송
│   ├── msgpack_adc.c / .h            ← MessagePack 인코딩
│   ├── tcp_gateway.c / .h            ← TCP Modbus 게이트웨이
│   ├── rs232.c / .h                  ← RS-232
│   ├── sync_gate.c / .h
│   └── time_helper.c / .h
└── docs/
    └── wifi_development.md           ← 이 파일
```

---

## 3. 빌드

### 3.1 west build 명령

```powershell
cd C:\nxp\Zephyr\nxp_zephyr

west build -b frdm_mcxn947/mcxn947/cpu0 zephyr/SmartGateway `
    -- -DCONF_FILE=prj.conf
```

> **팁:** MCUXpresso IDE에서 CMakePresets.json으로 관리하는 경우  
> IDE에서 빌드 타겟을 `frdm_mcxn947_mcxn947_cpu0`로 선택 후 Build.

### 3.2 빌드 결과물

```
build/zephyr/zephyr.elf   ← 디버그용
build/zephyr/zephyr.bin   ← 플래시용
build/zephyr/zephyr.hex   ← J-Link / LinkServer용
```

---

## 4. 플래시 및 디버그

### 4.1 west flash (J-Link / LinkServer)

```powershell
west flash
```

또는 특정 runner 지정:

```powershell
west flash --runner linkserver
west flash --runner jlink
```

### 4.2 시리얼 콘솔

FRDM-MCXN947을 USB로 PC에 연결하면 CDC-ACM 가상 COM 포트가 생성됨.

| 설정 | 값 |
|------|----|
| 보드 레이트 | 115200 |
| 데이터 비트 | 8 |
| 패리티 | None |
| 스톱 비트 | 1 |

터미널: PuTTY, Tera Term, VS Code Serial Monitor 등.

---

## 5. WiFi 개발 — 배경 및 목표

### 5.1 기존 구성

- 이더넷 (ENET) → 정적 IP `10.108.214.150`
- TCP Modbus 게이트웨이 (포트 20273)

### 5.2 추가 목표

- IW416 WiFi 모듈(SDIO)을 FRDM-MCXN947에 연결
- AP 접속 → 정적 IP 설정
- 내부 ADC 데이터를 WiFi를 통해 UDP로 전송

---

## 6. 하드웨어 연결

### 6.1 WiFi 모듈

```
FRDM-MCXN947
  └─ microSD 슬롯 (USDHC0 / SDIO 핀)
       └─ microSD 어댑터 (변환 보드)
            └─ EVK-MAYA-W166 J3 ZIF FPC 커넥터
                 (NXP IW416 WiFi/BT 칩)
```

**연결 신호 (6선):**

| 신호 | 설명 |
|------|------|
| CLK | SDIO 클럭 |
| CMD | 커맨드 |
| D0 ~ D3 | 데이터 4선 |
| VBUS_SDIO | 3.3V 전원 |

> **PDn(파워다운 핀):** EVK-MAYA-W166 내부 풀업 → HIGH 유지, GPIO 연결 불필요

### 6.2 ADC (검증용)

내부 LPADC0 — CH0A, CH0B (FRDM-MCXN947 헤더 핀)  
별도 하드웨어 불필요. 핀에 아무것도 연결 안 해도 플로팅 값으로 동작 확인 가능.

---

## 7. Device Tree Overlay 설정

파일: `boards/frdm_mcxn947_mcxn947_cpu0.overlay`

### 7.1 USDHC0 (SDIO WiFi)

```dts
&usdhc0 {
    status = "okay";
    /delete-property/ cd-gpios;   /* SD 카드 감지 핀 제거 — 없으면 초기화 실패 */
    max-bus-freq = <1000000>;      /* SDIO 초기화 주파수 1MHz */
    /delete-node/ sdmmc;           /* SD 카드 노드 제거 */

    nxp_wifi: nxp-wifi {
        compatible = "nxp,wifi";
    };
};
```

### 7.2 내부 LPADC0

```dts
&lpadc0 {
    status = "okay";
    /* 채널 설정은 adc.c에서 adc_channel_setup()으로 처리 */
};
```

### 7.3 AD7327 외부 SPI (예정 — 현재 주석)

```dts
/* &flexcomm6_lpspi6 {
 *     status = "okay";
 *     ad7327: ad7327@0 {
 *         compatible = "vnd,spi-device";
 *         reg = <0>;
 *         spi-max-frequency = <10000000>;
 *     };
 * }; */
```

---

## 8. prj.conf 핵심 설정

```conf
# ── SmartGateway 앱 ──────────────────────────────────────────
CONFIG_SMARTGATEWAY_LINE_ID="KRWJARDU001"
CONFIG_SMARTGATEWAY_UDP_PEER_IP="192.168.5.78"     # PC WiFi IP
CONFIG_SMARTGATEWAY_UDP_PEER_PORT=20263
CONFIG_SMARTGATEWAY_UDP_BIND_PORT=20263
CONFIG_SMARTGATEWAY_UDP_TEST_MODE=y

# ── WiFi ─────────────────────────────────────────────────────
CONFIG_SMARTGATEWAY_WIFI_ENABLE=y
CONFIG_SMARTGATEWAY_WIFI_SSID="TEIA-R&D-Center"
CONFIG_SMARTGATEWAY_WIFI_PSK="teia0323!"
CONFIG_SMARTGATEWAY_WIFI_STATIC_IP="192.168.5.100"
CONFIG_SMARTGATEWAY_WIFI_STATIC_NETMASK="255.255.255.0"
CONFIG_SMARTGATEWAY_WIFI_STATIC_GW="192.168.5.1"

CONFIG_WIFI=y
CONFIG_WIFI_NXP=y
CONFIG_NXP_MONOLITHIC_WIFI=y
CONFIG_NXP_IW416=y
CONFIG_NXP_IW416_UBX_MAYA_W1_USD=y
CONFIG_NXP_IW416_REGION_WW=y
CONFIG_WIFI_NM_WPA_SUPPLICANT=y
CONFIG_WIFI_NM_WPA_SUPPLICANT_THREAD_STACK_SIZE=14336
CONFIG_HEAP_MEM_POOL_SIZE=102400

# ── 네트워크 ─────────────────────────────────────────────────
CONFIG_NETWORKING=y
CONFIG_NET_IPV4=y
CONFIG_NET_IF_MAX_IPV4_COUNT=2     # 이더넷 + WiFi 동시 IPv4
CONFIG_NET_UDP=y
CONFIG_NET_TCP=y
CONFIG_NET_SOCKETS=y
CONFIG_NET_L2_ETHERNET=y
CONFIG_NET_DHCPV4=y
CONFIG_NET_CONFIG_SETTINGS=y
CONFIG_NET_CONFIG_INIT_TIMEOUT=0   # 부팅 시 30초 대기 제거
CONFIG_NET_CONFIG_MY_IPV4_ADDR="10.108.214.150"    # 이더넷 IP
CONFIG_NET_CONFIG_MY_IPV4_GW="10.108.214.1"
CONFIG_NET_CONFIG_MY_IPV4_NETMASK="255.255.255.0"

# ── ADC ──────────────────────────────────────────────────────
CONFIG_ADC=y           # 내부 LPADC0 검증 모드
# CONFIG_SPI=y         # AD7327 외부 SPI 사용 시 교체

# ── 로그 ─────────────────────────────────────────────────────
CONFIG_LOG=y
CONFIG_LOG_BUFFER_SIZE=8192
CONFIG_LOG_DEFAULT_LEVEL=3
```

---

## 9. WiFi Manager 구현

파일: `src/wifi_manager.c`, `src/wifi_manager.h`

### 9.1 설계 원칙

- `wifi_task`가 AP 연결 → 정적 IP 설정까지 담당
- 세마포어(`sem_wifi_ready`)로 완료를 main()에 통보
- main()은 WiFi 준비 완료 후에만 ADC/UDP 태스크 시작

### 9.2 동작 흐름

```
main()
  ├─ wifi_task_start()
  └─ wifi_wait_ready(30000ms) ──┐
                                │  [wifi_task]
                                ├─ net_if_get_first_wifi()
                                ├─ net_mgmt_add_event_callback()
                                ├─ k_sleep(2s)  ← wpa_supplicant 준비 대기
                                ├─ net_mgmt(NET_REQUEST_WIFI_CONNECT)
                                ├─ k_sem_take(&sem_connected, 30s)
                                ├─ wifi_set_static_ip()
                                │    ├─ net_if_ipv4_addr_add()
                                │    ├─ net_if_ipv4_set_netmask_by_addr()
                                │    ├─ net_if_ipv4_set_gw()
                                │    └─ net_if_set_default()  ← WiFi가 기본 경로
                                └─ k_sem_give(&sem_wifi_ready)
  ├─ adc_task_start()
  └─ udp_task_start()
```

### 9.3 AP 연결 파라미터 핵심

```c
params.band = WIFI_FREQ_BAND_UNKNOWN;  /* 2.4/5GHz 모두 허용 */
```

> `WIFI_FREQ_BAND_2_4_GHZ`로 설정하면 5GHz AP에 연결 안 됨.  
> `WIFI_FREQ_BAND_ANY`는 이 Zephyr 버전에 없음.  
> `WIFI_FREQ_BAND_UNKNOWN`이 wpa_supplicant에서 "밴드 제한 없음"으로 처리됨. (`modules/hostap/src/supp_api.c:667` 참조)

### 9.4 이벤트 콜백 타입 주의

```c
/* uint32_t 아닌 uint64_t — 틀리면 런타임 오동작 */
static void on_wifi_event(struct net_mgmt_event_callback *cb,
                          uint64_t event, struct net_if *iface)
```

---

## 10. 이더넷 + WiFi 동시 운용

| 인터페이스 | IP | 서브넷 | 용도 |
|------------|-----|--------|------|
| 이더넷 | 10.108.214.150 | 10.108.214.0/24 | TCP Modbus 게이트웨이 |
| WiFi (IW416) | 192.168.5.100 | 192.168.5.0/24 | ADC 데이터 UDP 전송 |

**필수 설정:**
```conf
CONFIG_NET_IF_MAX_IPV4_COUNT=2
# 기본값 1이면 두 번째 인터페이스에 IPv4 주소 추가 불가
# → net_if_ipv4_addr_add() 반환값 NULL
```

`net_if_set_default(wifi_iface)` 호출로 WiFi를 기본 인터페이스로 지정해야 UDP `sendto()`가 WiFi 경로를 선택한다.

---

## 11. ADC → UDP 파이프라인

```
[LPADC0 CH0A/CH0B]
    │  2ms 샘플링
    ▼
[adc_task]  2초 윈도우 min/max 추적
    │  adc_has_sample = true (2초 후)
    ▼
[adc_get_latest()]
    ▼
[udp_task]  MessagePack 인코딩
    ▼
[zsock_sendto()]  192.168.5.78:20263
    ▼
[PC WiFi 수신]
```

**UDP 패킷 형식 (테스트 모드, MessagePack array):**
```
[ "KRWJARDU001", "20260428153045123", 1.23, 0.87 ]
  (라인 ID)       (타임스탬프 17자)    (CH0V) (CH1V)
```

---

## 12. RAM 최적화 이력

WiFi 드라이버는 RAM을 많이 사용하여 여러 차례 overflow가 발생했다.

| 증상 | 원인 | 해결 |
|------|------|------|
| noinit 섹션 808B 초과 | wpa_supplicant 스택 기본값 과다 | 스택 16384 → 14336 |
| wifi_task 런타임 crash | wifi_task 스택 3072B 부족 | 4096B로 복구 |
| RAM 3720B 초과 | NET_IF_MAX_IPV4_COUNT=2 + LOG_BUFFER 16KB | LOG_BUFFER 16384 → 8192 |

**최종 스택 배분:**

| 태스크 | 스택 |
|--------|------|
| main | 2048 B |
| wifi_task | 4096 B |
| wpa_supplicant | 14336 B |
| adc_task | 1536 B |
| udp_task | 2048 B |

---

## 13. 주요 트러블슈팅

| 증상 | 원인 | 해결 |
|------|------|------|
| WiFi 드라이버 초기화 실패 | overlay에 `cd-gpios` 프로퍼티 존재 | `/delete-property/ cd-gpios;` 추가 |
| 부팅 후 30초 지연 | NET_CONFIG_SETTINGS가 이더넷 대기 | `CONFIG_NET_CONFIG_INIT_TIMEOUT=0` |
| `net_if_ipv4_addr_add()` NULL 반환 | NET_IF_MAX_IPV4_COUNT=1 (기본) | `CONFIG_NET_IF_MAX_IPV4_COUNT=2` |
| wifi_task 런타임 crash | 스택 3072B 부족 | 4096B로 복구 |
| `WIFI_FREQ_BAND_ANY` 미정의 | 이 버전에 상수 없음 | `WIFI_FREQ_BAND_UNKNOWN` 사용 |
| UDP "No ADC data yet" 반복 | ADC 2초 윈도우 완료 전 | 2초 대기 후 정상 동작 |

---

## 14. 부팅 후 정상 로그 예시

```
*** Smart Gateway V2.7.1 ***
Board: frdm_mcxn947/mcxn947/cpu0
Network: WiFi (IW416 / EVK-MAYA-W166)
UDP: peer 192.168.5.78:20263
TCP GW: listen 0.0.0.0:20273 | board IP 10.108.214.150

[MAIN] WiFi 연결 대기 중...
[wifi_mgr] WiFi 태스크 시작
[wifi_mgr] AP 연결 중: 'TEIA-R&D-Center' (보안=1) ...
[wifi_mgr] WiFi connected: SSID=TEIA-R&D-Center
[wifi_mgr] WiFi 정적 IP: 192.168.5.100 / 255.255.255.0  GW: 192.168.5.1
[wifi_mgr] === WiFi 준비 완료 ===
[MAIN] WiFi 준비 완료 — ADC/UDP 시작
[ADC] LPADC0 시작: 2ch, 2ms 사이클, 2000ms 윈도우
[UDP] Socket created (fd=3)
[UDP] TX target: 192.168.5.78:20263, interval 2000ms
```

---

## 15. 다음 단계

| 항목 | 내용 |
|------|------|
| WiFi UDP 검증 | PC에서 포트 20263 수신 확인 (Wireshark 또는 Python `socket.recvfrom`) |
| AD7327 전환 | 아래 "전환 방법" 참조 |
| TCP 게이트웨이 활성화 | `main.c`에서 `tcp_gateway_task_start()` 주석 해제 |
| RS-232 활성화 | `main.c`에서 `rs232_task_start()` 주석 해제 |

---

## 16. LPADC0 ↔ AD7327 전환 방법

### LPADC0 → AD7327

**prj.conf:**
```conf
# CONFIG_ADC=y        ← 주석 처리
CONFIG_SPI=y           ← 활성화
```

**overlay** — AD7327 블록 주석 해제:
```dts
&flexcomm6_lpspi6 {
    status = "okay";
    ad7327: ad7327@0 {
        compatible = "vnd,spi-device";
        reg = <0>;
        spi-max-frequency = <10000000>;
    };
};
```

**adc.c** — 하단 `[AD7327 disabled]` 주석 블록을 실제 코드로 복구, LPADC0 코드 주석 처리.

### AD7327 → LPADC0

위 과정의 역순.
