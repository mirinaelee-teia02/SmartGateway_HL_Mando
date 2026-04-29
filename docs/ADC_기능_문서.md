# Smart Gateway ADC 기능 문서

## 1. 개요

Smart Gateway 프로젝트의 ADC(Analog-to-Digital Converter) 모듈은 **FRDM-MCXN947** 보드의 LPADC(Low-Power ADC)를 사용하여 아날로그 전압을 디지털 값으로 변환합니다.

### 1.1 주요 특징

| 항목 | 내용 |
|------|------|
| **채널 수** | 2채널 (ADC0_A0, ADC0_B0) |
| **해상도** | 12-bit (0~4095) |
| **입력 범위** | 0~3.3V |
| **샘플링 주기** | 2ms (500 samples/sec) |
| **출력 주기** | 200ms마다 UART 출력 (100샘플마다) |

---

## 2. 하드웨어 사양

### 2.1 대상 보드

- **보드**: NXP FRDM-MCXN947
- **MCU**: MCXN947 (Cortex-M33, dual-core)
- **ADC 컨트롤러**: LPADC (Low-Power ADC)

### 2.2 핀 연결 (Arduino UNO R3 헤더 기준)

| J4 핀 번호 | Arduino 핀 | LPADC 채널 | 내부 신호명 | 용도 |
|------------|------------|------------|-------------|------|
| Pin 2 | A0 | CH0A | ADC0_A0 | 아날로그 입력 채널 0 |
| Pin 4 | A1 | CH0B | ADC0_B0 | 아날로그 입력 채널 1 |

> **참고**: J4는 2x6 커넥터로, Arduino UNO R3 실드 보드와 호환됩니다.

### 2.3 전기적 사양

- **기준 전압 (VREF)**: ADC_REF_EXTERNAL1 (3.3V)
- **입력 전압 범위**: 0V ~ 3.3V
- **12-bit 변환**: 0V → 0, 3.3V → 4095
- **전압 변환식**: `V(mV) = (raw × 3300) / 4095`

---

## 3. 소프트웨어 구조

### 3.1 파일 구성

```
SmartGateway/
├── src/
│   ├── main.c      # 메인 진입점, ADC 태스크 시작
│   ├── adc.c       # ADC 모듈 구현
│   └── adc.h       # ADC 모듈 헤더 (API 선언)
├── boards/
│   └── frdm_mcxn947_mcxn947_cpu0.overlay  # 보드별 오버레이 (현재 비어 있음)
├── prj.conf        # Kconfig 설정
└── CMakeLists.txt  # 빌드 설정
```

### 3.2 모듈 구조

```
┌─────────────┐     adc_task_start()      ┌─────────────┐
│   main.c    │ ─────────────────────────► │   adc.c     │
│             │                            │             │
│ - 초기화    │                            │ - adc_task  │
│ - ADC 시작  │                            │   (스레드)   │
└─────────────┘                            │ - 채널 설정  │
                                           │ - ADC 읽기   │
                                           └──────┬──────┘
                                                  │
                                                  ▼
                                           ┌─────────────┐
                                           │ Zephyr ADC  │
                                           │ (LPADC 드라이버)│
                                           └─────────────┘
```

---

## 4. 설정 (Configuration)

### 4.1 prj.conf

```kconfig
CONFIG_CONSOLE=y          # UART 콘솔 (printf 출력용)
CONFIG_ADC=y              # ADC 서브시스템 활성화
CONFIG_MAIN_STACK_SIZE=1024   # 메인 스레드 스택 (ADC 태스크는 별도 1024 스택 사용)
CONFIG_REGULATOR=y        # VREF 레귤레이터 (ADC_REF_EXTERNAL1 필수)
```

| 설정 | 설명 |
|------|------|
| `CONFIG_ADC` | Zephyr ADC API 및 LPADC 드라이버 활성화 |
| `CONFIG_REGULATOR` | ADC 기준 전압(3.3V)을 위한 NXP VREF 레귤레이터. `ADC_REF_EXTERNAL1` 사용 시 **필수** |
| `CONFIG_MAIN_STACK_SIZE` | main() 스레드 스택. ADC 태스크는 별도 스택 사용 |

### 4.2 Overlay

`boards/frdm_mcxn947_mcxn947_cpu0.overlay`는 현재 비어 있으며, 기본 보드 설정을 사용합니다. ADC 핀(ADC0_A0, ADC0_B0)은 보드 기본 pinctrl에 정의되어 있습니다.

---

## 5. API 레퍼런스

### 5.1 adc_task_start()

```c
int adc_task_start(void);
```

**설명**: ADC 샘플링을 수행하는 전용 스레드를 생성하고 시작합니다.

**반환값**:
- `0`: 성공
- `-1`: 스레드 생성 실패

**호출 예**:
```c
if (adc_task_start() != 0) {
    printf("[MAIN] Failed to create ADC task\n");
    return -1;
}
```

---

## 6. 구현 상세

### 6.1 LPADC 채널 매핑

NXP LPADC는 다중 채널을 지원하며, `input_positive` 값으로 입력을 선택합니다.

| 채널 | input_positive | LPADC 내부 | J4 핀 |
|------|----------------|------------|--------|
| 0 | 0x00 | CH0A (ADC0_A0) | Pin 2 |
| 1 | 0x20 | CH0B (ADC0_B0) | Pin 4 |

```c
/* adc.c 내부 */
static const uint8_t adc_inputs[] = { 0, 0x20 };  /* CH0A, CH0B */
```

### 6.2 채널 설정 (adc_channel_cfg)

```c
struct adc_channel_cfg ch_cfg = {
    .gain             = ADC_GAIN_1,        // 이득 1 (무변환)
    .reference        = ADC_REF_EXTERNAL1, // 외부 기준 3.3V
    .acquisition_time = ADC_ACQ_TIME_DEFAULT,
    .channel_id       = i,                 // 채널 인덱스 0, 1
    .differential     = 0,                 // 단일 종단 (Single-ended)
    .input_positive   = adc_inputs[i],    // ADC0_A0 또는 ADC0_B0
};
```

### 6.3 샘플링 주기

- **ADC_READ_INTERVAL_MS = 2**: 2ms마다 `adc_read()` 호출
- **ADC_PRINT_EVERY_N = 100**: 100번째 샘플마다 UART로 출력
- **실제 출력 간격**: 2ms × 100 = **200ms**

> UART 전송 속도 제한으로 인해 매 샘플마다 출력하면 2ms 주기를 유지할 수 없어, 100샘플마다 출력하도록 설계되었습니다.

### 6.4 전압 변환

12-bit ADC (0~4095)를 0~3300mV로 변환:

```c
int32_t mv = (raw * (int32_t)VREF_MV) / 4095;
```

- `raw`: ADC 원시값 (0~4095)
- `VREF_MV`: 3300 (3.3V)
- `mv`: 밀리볼트 (0~3300)

---

## 7. 사용 방법

### 7.1 빌드

```bash
cd SmartGateway
west build -b frdm_mcxn947/mcxn947/cpu0 -d debug
```

### 7.2 플래시 및 실행

```bash
west flash
```

### 7.3 시리얼 모니터 (출력 확인)

- **보드레이트**: 115200 (기본)
- **출력 형식**:
  ```
  [ADC] Ch0    Ch1   |  V0     V1   
  ----------------------------------
  [ADC] 2048   1024  | 1.650  0.825 
  ```

---

## 8. 출력 형식

| 필드 | 설명 |
|------|------|
| Ch0, Ch1 | ADC 원시값 (0~4095) |
| V0, V1 | 변환된 전압 (V.xxx 형식, 단위: V) |

---

## 9. 트러블슈팅

### 9.1 adc_channel_setup 실패 (에러 -22, EINVAL)

**원인**: `input_positive` 값이 보드에서 지원하지 않거나, pinctrl에 ADC 핀이 정의되지 않음.

**조치**:
- 스키매틱과 `adc_inputs[]` 매핑 확인
- `CONFIG_REGULATOR=y` 설정 확인
- 보드 overlay에서 ADC 핀 pinctrl 추가 검토

### 9.2 Device not ready

**원인**: LPADC 디바이스 초기화 실패.

**조치**:
- `CONFIG_ADC=y`, `CONFIG_REGULATOR=y` 확인
- 보드 전원 및 리셋 확인

### 9.3 샘플링 주기 변경

- **읽기 주기**: `ADC_READ_INTERVAL_MS` 수정 (단위: ms)
- **출력 주기**: `ADC_PRINT_EVERY_N` 수정 (N번째 샘플마다 출력)

### 9.4 기준 전압 변경

- `ADC_REF_INTERNAL` (내부 기준) 사용 시: `CONFIG_REGULATOR` 제거 가능
- `VREF_MV`를 실제 기준 전압(mV)에 맞게 수정

---

## 10. 참고 자료

- **Zephyr ADC API**: `zephyr/drivers/adc.h`
- **NXP LPADC**: MCUXpresso SDK LPADC 드라이버
- **FRDM-MCXN947**: [NXP 공식 페이지](https://www.nxp.com/design/development-boards/frdm-mcxn947)

---

*문서 버전: 1.0*  
*최종 수정: 2026.03*
