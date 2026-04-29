@echo off
REM SmartGateway 빌드 스크립트
REM 사용법: build.bat 또는 이 스크립트가 있는 폴더에서 실행

cd /d "%~dp0"

REM 방법 1: west build (권장)
echo Building with west...
west build -b frdm_mcxn947/mcxn947/cpu0 -d debug
if %ERRORLEVEL% NEQ 0 (
    echo Build failed with west. Exit code: %ERRORLEVEL%
    exit /b %ERRORLEVEL%
)

echo Build completed successfully.
exit /b 0
