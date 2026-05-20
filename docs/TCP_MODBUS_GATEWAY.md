# Smart Gateway — TCP/IP Modbus 게이트웨이 명세

본 문서는 Zephyr **SmartGateway** 애플리케이션에서 구현한 **TCP ↔ RS-232(Modbus RTU)** 중계와 관련 **Kconfig·동작 흐름·외곽 프레임 포맷**을 정리합니다. 구현 소스 기준 경로는 `src/tcp_gateway.c`, `src/sync_gate.c`, `src/time_helper.c`, `src/udp.c` 입니다.

---

## 1. 목적과 역할

- **이더넷 TCP**로 수신한 명령(외곽 프로토콜)을 해석한 뒤, **RS-232(UART)**로 **Modbus RTU** 바이트열을 송신하고, 슬레이브 응답을 다시 TCP로 되돌려 보냅니다.
- **외곽 프레임에는 CRC가 없습니다.** 길이 필드와 ETX로 경계를 잡습니다.
- TCP Modbus 게이트웨이가 켜진 빌드(`CONFIG_SMARTGATEWAY_TCP_MODBUS_GATEWAY=y`)에서는 **RS-232용 UDP 패스스루 백그라운드 스레드를 띄우지 않습니다** (Kconfig 주석과 동일). Modbus 중계가 RS-232 채널을 전용으로 사용합니다.

---

## 2. 전제 조건 (Kconfig / 빌드)

| 기호 | 의미 |
|------|------|
| `CONFIG_SMARTGATEWAY_RS232_ENABLE` | TCP 게이트웨이는 이 옵션이 켜져 있어야 함 (`depends on`). |
| `CONFIG_SMARTGATEWAY_TCP_MODBUS_GATEWAY` | 본 TCP 프로토콜 스택 활성화. `NET_TCP` 선택. |
| `CONFIG_SMARTGATEWAY_TCP_CLIENT_MODE` | **y**: 보드가 `connect`로 나감(클라이언트). **n**: `bind`+`listen`+`accept`(서버). |

서버 모드에서만 사용:

- `CONFIG_SMARTGATEWAY_TCP_BIND_IP` — 예: `0.0.0.0` (모든 인터페이스)
- `CONFIG_SMARTGATEWAY_TCP_LISTEN_PORT`

클라이언트 모드에서만 사용:

- `CONFIG_SMARTGATEWAY_TCP_PEER_IP` — 접속 대상(예: PC에서 SerialPortMon TCP Server)
- `CONFIG_SMARTGATEWAY_TCP_PEER_PORT`
- `CONFIG_SMARTGATEWAY_TCP_RECONNECT_MS` — `connect` 실패 또는 세션 종료 후 재시도 간격

공통:

- `CONFIG_SMARTGATEWAY_TCP_HANDSHAKE_POLL_MS` — 핸드셰이크 단계에서 0x80 전송 후 **poll 대기 시간(ms)** (기본 2000).
- `CONFIG_SMARTGATEWAY_TCP_MAX_BODY` — Body 최대 길이(0~256, 기본 256).
- `CONFIG_SMARTGATEWAY_TCP_STREAM_BUF` — TCP 스트림 조립용 수신 버퍼. **최소 권장: 5 + MAX_BODY + 2**.
- `CONFIG_SMARTGATEWAY_TCP_RS232_RX_TIMEOUT_MS`, `CONFIG_SMARTGATEWAY_TCP_RS232_INTER_FRAME_GAP_MS` — `rs232` 모듈로 전달되는 Modbus 수신 동작 파라미터.

---

## 3. TCP 역할(서버 vs 클라이언트)

### 3.1 서버 모드 (`TCP_CLIENT_MODE` = n)

1. `socket` → `SO_REUSEADDR` → `bind(BIND_IP, LISTEN_PORT)` → `listen`
2. 루프에서 `accept` → 수락된 fd마다 세션 처리(`gw_handle_client`)
3. 세션이 끝나면 해당 fd `close`, 다시 `accept`

### 3.2 클라이언트 모드 (`TCP_CLIENT_MODE` = y)

1. 무한 루프: `socket` → `connect(PEER_IP, PEER_PORT)`
2. 성공 시 `gw_handle_client` (서버 모드와 **동일한 프로토콜**)
3. 세션 종료 또는 오류 후 fd `close` → `RECONNECT_MS` 만큼 sleep → 다시 `connect` 시도

**중요:** 소켓의 listen/connect 역할만 바뀌고, 연결 수립 이후의 **메시지 타입·프레임 포맷·핸드셰이크**는 동일합니다.

---

## 4. 외곽 프레임 바이너리 포맷

한 프레임의 총 길이:

**5 + N + 2** 바이트 (N = Body 바이트 수, `Length` 필드와 동일)

| 오프셋 | 크기 | 필드 | 설명 |
|--------|------|------|------|
| 0 | 1 | STX | 고정 `0x55` |
| 1–2 | 2 | Length | Body 길이 N, **big-endian** |
| 3 | 1 | MsgType | 메시지 종류 (아래 절 참고) |
| 4 | 1 | Seq | 요청/응답 짝 맞춤(특히 0x01 ↔ 0x81) |
| 5 … 5+N−1 | N | Body | N=0이면 없음 |
| 5+N | 1 | Error | 정상 응답은 보통 `0`; UART 실패 시 `0xE3` 등 |
| 5+N+1 | 1 | ETX | 고정 `0x03` |

검증 실패 시 동작 요약:

- **핸드셰이크 루프:** 비정상 Length 등은 STX 한 바이트만 슬라이드하여 재동기.
- **본 세션 루프:** Length가 `GW_MAX_BODY` 초과면 스트림 전체 리셋(`rlen=0`). 기타 파싱 실패는 STX 1바이트 밀기.

---

## 5. MsgType 정의

| 값 | 이름 | 프레임 | 방향 | Body |
|----|------|--------|------|------|
| `0x80` | CONNECT | compact | 보드 → 서버 | 장치코드(ASCII 가변) + IDX(1) + 버전(3, 예 2.0.0) |
| `0x01` | SYNC | compact | 서버 → 보드 | 시각(9) + RS-232(5) |
| `0x81` | ACK | compact | 보드 → 서버 | Ret(1), `0x00`=OK |
| `0x02` | MODBUS_REQ | Len+STX | 서버 → 보드 | Modbus RTU ADU |
| `0x82` | MODBUS_RESP | Len+STX | 보드 → 서버 | 시각(9) + Modbus RTU 응답 |

compact = `STX + MsgType + Seq + Body + ETX`. Len = `STX + Len(2BE) + MsgType + Seq + Body + ETX`.

기타 MsgType은 로그 후 프레임만 소비(무시).

---

## 6. 연결 후 처리 순서 (세션 공통)

구현상 `gw_handle_client(int cfd)` 단일 경로:

### 6.1 핸드셰이크 (`gw_tcp_handshake`)

1. **0x80 프레임** 조립 후 `send` (전체 전송은 `gw_send_all`로 부분 송신 처리).
2. `zsock_poll(..., HANDSHAKE_POLL_MS)` 로 수신 대기.
3. 수신 바이트를 누적 버퍼에 붙이고 `gw_process_frames_handshake`로 파싱.
4. **MsgType `0x01`(SYNC)** compact 프레임이 파싱되면 RS-232 설정·`0x81` ACK 후 핸드셰이크 성공.
5. poll 타임아웃이면 **다시 1번부터** (`HANDSHAKE_POLL_MS`, 기본 2000ms).

### 6.2 SYNC(0x01) Body — 시각 및 UDP 게이트

`sync_gate.c` → `time_helper.c`:

- Body **14바이트**: 시각 9 + RS-232 설정 5. compact 프레임 전체 **18바이트** (헤더 3 + Body 14 + ETX 1).
- 시각 파싱은 앞 **9바이트**만 사용.
- `body[0..1]`: 연도 **big-endian uint16**
- `body[2]`: 월 (1–12)
- `body[3]`: 일  
- `body[4..6]`: 시·분·초  

`timeutil_timegm`으로 UTC epoch로 바꾼 뒤 내부 벽시계 동기 플래그를 세우고, `sg_timesync_from_tcp_notify`에서 **`sg_time_ok = true`** 로 두어 **UDP 수신 허용**으로 전환합니다.

`udp.c`: **TCP 0x01 SYNC 처리 전**까지 `sg_udp_rx_allowed()`는 false.

### 6.3 MODBUS_REQ(0x02)

1. Len 프레임으로 수신. Body가 비어 있으면 스킵.
2. `rs232_modbus_txrx(1, …)` — UART1(FC5).
3. 실패 시: MsgType `0x82`, Body = 시각(9)만.
4. 성공 시: MsgType `0x82`, Body = 시각(9) + Modbus RTU 응답.
5. 응답 전송 후 **`k_msleep(10)`** (연속 트랜잭션 부하 완화).

---

## 7. TCP 스트림 처리

Berkeley 소켓은 **메시지 경계를 보장하지 않습니다.**  
구현은 **고정 배열 `gw_rx_buf`**에 수신 데이터를 append하고, 앞에서부터:

1. `0x55`(STX) 탐색, 앞 쓰레기는 `memmove`로 제거  
2. `Length`로 `need = 5 + N + 2` 계산  
3. `rlen < need` 이면 다음 `recv` 대기  
4. 충분히 모이면 `gw_parse_frame` 검증 후 처리, 처리한 만큼 버퍼에서 제거  

`gw_parse_frame` 실패 **reason** 코드(로그용):

1. total 너무 짧음  
2. STX 아님  
3. Length > MAX_BODY  
4. total ≠ Length 기대치  
5. 마지막 바이트 ETX 아님  

---

## 8. 송신 오류와 errno 참고

`gw_send_all`은 `zsock_send` 실패 시 `errno`를 그대로 로그합니다.

- Zephyr minimal libc에서 **`errno == 128`은 `ENOTCONN`(Socket is not connected)** 입니다.  
  상대가 연결을 닫은 뒤 송신하거나, 스택이 연결 끊김으로 판단한 경우에 발생할 수 있습니다.

---

## 9. 스레드와 진입점

- `tcp_gateway_task_start()` (`tcp_gateway.h`): 우선순위 5로 스레드 `tcp_gateway` 생성, 스택 **6144** 바이트.
- `main.c`에서 ADC·RS-232·UDP 태스크 다음에 호출하는 구성이 일반적입니다.

---

## 10. PC/도구 연동 예 (SerialPortMon)

- PC에서 **TCP Server**로 listen (예: `0.0.0.0`, 포트 `20273`).
- 보드는 **클라이언트 모드**로 `PEER_IP`/`PEER_PORT`에 PC를 지정.
- 핸드셰이크에서 PC는 규격에 맞는 **0x00 TIMESYNC** 프레임을 보내야 UDP 게이트 및 시각동기가 동작합니다.

---

## 11. 관련 파일 목록

| 파일 | 역할 |
|------|------|
| `src/tcp_gateway.c` | 소켓 서버/클라이언트, 프레임 파싱, 핸드셰이크, Modbus 중계 |
| `src/tcp_gateway.h` | `tcp_gateway_task_start()` |
| `src/sync_gate.h` / `sync_gate.c` | TIMESYNC 시 UDP RX 허용 플래그 |
| `src/time_helper.c` | TIMESYNC Body 파싱 및 UTC 동기 |
| `src/rs232.c` (및 헤더) | Modbus TX/RX, 타임아웃/프레임 간격 Kconfig 연동 |
| `src/udp.c` | `sg_udp_rx_allowed()`와 연동된 UDP RX 게이트 |
| `Kconfig` | 위 모든 `SMARTGATEWAY_TCP_*` 옵션 정의 |

---

*문서는 저장소의 해당 소스와 `Kconfig`를 기준으로 작성되었습니다. 옵션 기본값이 바뀌면 본 문서의 표와 실제 menuconfig를 함께 확인하십시오.*
