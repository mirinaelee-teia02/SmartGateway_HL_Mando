# Smart Gateway - UDP Task & ADC 데이터 전달 설계

## 1. 개요

- **ADC Task**: 2~8채널 샘플링 (현재 2채널, 2ms 주기, 500 samples/sec)
- **UDP Task**: 이더넷을 통한 ADC 데이터 전송

---

## 2. ADC → UDP 데이터 전달 방안 비교

### 2.1 메시지 큐 (k_msgq) 단점
| 항목 | 내용 |
|------|------|
| 8채널 @500Hz | 8×2B×500 = **8 KB/s** |
| 큐 깊이 | 샘플 단위 저장 시 오버헤드 큼, 패킷 단위로 하면 지연 증가 |
| 속도 불일치 | ADC가 빠르고 UDP가 느리면 큐 오버플로우 |

### 2.2 권장: **공유 버퍼 + 최신값 스냅샷** (이중 버퍼)

```
┌─────────────┐                    ┌─────────────┐
│  ADC Task   │  버퍼 A/B 스왑      │  UDP Task   │
│  (Producer) │ ─────────────────► │  (Consumer) │
└─────────────┘    semaphore       └─────────────┘
```

**장점**:
- 메모리 효율: 8채널×2B×2버퍼 = **32 bytes** (매우 작음)
- 지연 최소: UDP는 "현재 최신값"만 전송
- 동기화 단순: 세마포어 또는 스핀락으로 스왑만 보호
- 8채널까지 용이하게 확장 가능

**데이터 구조 예시**:
```c
typedef struct {
    uint32_t timestamp_ms;   /* 샘플 시각 (선택) */
    uint16_t ch[8];          /* ADC raw (0~4095), 최대 8채널 */
    uint8_t  ch_count;       /* 실제 채널 수 (2~8) */
} adc_snapshot_t;
```

### 2.3 대안: 링 버퍼 (k_ringbuf) - 배치 전송용
- **용도**: 연속 N샘플을 한 패킷으로 전송할 때
- 500Hz × 8ch × 2B = 8KB/s → 100ms 주기면 800 bytes/패킷
- 샘플 히스토리가 필요할 때 유용

---

## 3. 아키텍처

```
┌───────────────────────────────────────────────────────────────────┐
│                         Smart Gateway                              │
│  ┌─────────────┐     adc_get_latest()      ┌─────────────────────┐│
│  │  ADC Task   │ ─────────────────────────►│     UDP Task         ││
│  │  - 2ms 샘플  │   adc_snapshot_t (공유)   │  - 이더넷 초기화      ││
│  │  - 버퍼 갱신  │                          │  - UDP 소켓          ││
│  └─────────────┘                          │  - 주기적 전송        ││
│                                           └──────────┬──────────┘│
└───────────────────────────────────────────────────────┼──────────┘
                                                        │ UDP
                                                        ▼
                                               ┌─────────────────┐
                                               │  PC / 서버       │
                                               │  (수신 IP:PORT)  │
                                               └─────────────────┘
```

---

## 4. UDP 패킷 포맷: **MessagePack**

ADC 스냅샷을 MessagePack map으로 직렬화합니다.

**형식**: `{"line": N, "datetime": [yy,mm,dd,hh,mm,ss], "raw": [...], "min": [...], "max": [...]}`

| 필드 | 타입 | 설명 |
|------|------|------|
| line | uint32 | 라인정보 (패킷 시퀀스) |
| datetime | [6] | 년/월/일/시간/분/초 |
| raw | [uint16×8] | ADC 8채널 RAW |
| min | [uint16×8] | 2초 윈도우 최소 |
| max | [uint16×8] | 2초 윈도우 최대 |

**전송 주기**: `UDP_SEND_INTERVAL_MS` (udp.h, 기본 2000ms)

**수신 예시 (Python)**:
```python
import msgpack, socket
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("0.0.0.0", 8888))
while True:
    data, _ = sock.recvfrom(256)
    d = msgpack.unpackb(bytes(data))
    print(d["line"], d["datetime"], d["raw"], d["min"], d["max"])
```

---

## 5. Task 우선순위 및 주기

| Task | 우선순위 | 주기 | 설명 |
|------|----------|------|------|
| ADC | 5 | 2ms | 샘플링 (기존 유지) |
| UDP | 4 | 50~200ms | 전송 주기 (설정 가능) |

UDP 전송 주기는 네트워크 부하와 실시간성 요구에 따라 조절 (예: 50ms=20Hz, 100ms=10Hz, 200ms=5Hz).

---

## 6. 설정 (Kconfig / overlay)

- `CONFIG_NETWORKING=y`
- `CONFIG_NET_IPV4=y`
- `CONFIG_NET_UDP=y`
- `CONFIG_NET_SOCKETS=y`
- 이더넷 드라이버 (MCXN947: `CONFIG_ETH_NXP_ENET` 또는 해당 SoC 설정)
- IP 주소: DHCP 또는 정적 IP (overlay에서 설정)

---

*버전: 1.0 | 2026.03*
