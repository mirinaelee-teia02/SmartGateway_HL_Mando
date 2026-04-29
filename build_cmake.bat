@echo off
REM SmartGateway - west 없이 CMake 직접 빌드
REM 사용법: build_cmake.bat (SmartGateway 폴더에서 실행)
REM
REM [필수 조건]
REM   - ZEPHYR_BASE 환경변수 설정, 또는 아래에서 자동 감지
REM   - CMake 3.20+, Ninja, Zephyr SDK (arm-zephyr-eabi 등)

cd /d "%~dp0"

REM ZEPHYR_BASE: SmartGateway의 부모 = zephyr 루트
if "%ZEPHYR_BASE%"=="" (
    set "ZEPHYR_BASE=%~dp0.."
    echo [INFO] ZEPHYR_BASE not set, using: %ZEPHYR_BASE%
)
REM CMake용: 백슬래시(\)를 슬래시(/)로 변환 - CMake에서 \Z 가 Invalid escape 오류 방지
set "ZEPHYR_BASE_CMAKE=%ZEPHYR_BASE:\=/%"

REM 빌드 디렉토리
set "BUILD_DIR=%~dp0debug"
set "BOARD=frdm_mcxn947/mcxn947/cpu0"

echo.
echo === SmartGateway CMake Build (no west) ===
echo ZEPHYR_BASE = %ZEPHYR_BASE_CMAKE%
echo BUILD_DIR   = %BUILD_DIR%
echo BOARD       = %BOARD%
echo.

REM CMake 구성: ZEPHYR_BASE를 슬래시(/)로 전달 (백슬래시 \Z 이스케이프 오류 방지)
echo [1/2] Configuring...
set "ZEPHYR_BASE=%ZEPHYR_BASE_CMAKE%"
cmake -S "%~dp0" -B "%BUILD_DIR%" ^
    -G Ninja ^
    -DBOARD=%BOARD% ^
    -DZEPHYR_BASE=%ZEPHYR_BASE_CMAKE%
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo [ERROR] CMake configuration failed.
    echo.
    echo [해결 방법]
    echo 1. ZEPHYR_BASE가 올바른지 확인: %ZEPHYR_BASE%
    echo    (zephyr 디렉터리, 즉 CMakeLists.txt 포함 폴더여야 함)
    echo 2. CMake 3.20 이상: cmake --version
    echo 3. Ninja 설치 확인: ninja --version
    echo 4. Zephyr SDK 환경변수: ZEPHYR_SDK_INSTALL_DIR
    echo.
    exit /b 1
)

REM 빌드
echo.
echo [2/2] Building...
cmake --build "%BUILD_DIR%"
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Build failed.
    exit /b 1
)

echo.
echo === Build completed successfully ===
echo Output: %BUILD_DIR%\zephyr\zephyr.elf
exit /b 0
