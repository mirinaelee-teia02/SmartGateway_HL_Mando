# SmartGateway - west 없이 CMake 빌드 가이드

## 1. 문제 원인 (Invalid character escape `\Z`)

Windows 경로 `C:\nxp\Zephyr\...`의 **백슬래시(`\`)**가 CMake 문자열에서 이스케이프 문자로 해석됨.
- `\Z` → 잘못된 이스케이프 시퀀스
- `\n` → 줄바꿈으로 해석
- 결과: `cmake_parse_arguments` Syntax error

### 해결
**ZEPHYR_BASE 경로를 슬래시(`/`)로 변환**하여 CMake에 전달.

---

## 2. west 없이 빌드 방법

### 방법 A: build_cmake.bat 사용 (권장)

```batch
cd C:\nxp\Zephyr\nxp_zephyr\zephyr\SmartGateway
build_cmake.bat
```

- `ZEPHYR_BASE` 미설정 시 자동으로 `../` (zephyr 루트) 사용
- 경로를 `/`로 변환 후 CMake에 전달

### 방법 B: 수동 CMake

```batch
cd C:\nxp\Zephyr\nxp_zephyr\zephyr\SmartGateway

REM 경로는 반드시 슬래시(/) 사용
set ZEPHYR_BASE=C:/nxp/Zephyr/nxp_zephyr/zephyr

cmake -S . -B debug -G Ninja ^
    -DBOARD=frdm_mcxn947/mcxn947/cpu0 ^
    -DZEPHYR_BASE=%ZEPHYR_BASE%
cmake --build debug
```

---

## 3. 필수 조건

| 항목 | 확인 |
|------|------|
| CMake 3.20+ | `cmake --version` |
| Ninja | `ninja --version` |
| Zephyr SDK | `ZEPHYR_SDK_INSTALL_DIR` 환경변수 |

---

## 4. 출력 파일

- `debug\zephyr\zephyr.elf` — 최종 펌웨어
- `debug\zephyr\zephyr.hex` — HEX (플래시용)

---

*버전: 1.0 | 2026.03*
