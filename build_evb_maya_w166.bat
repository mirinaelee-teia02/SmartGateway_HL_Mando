@echo off
REM SmartGateway: 기본 = FRDM-MCXN947 + EVK-MAYA-W166 (별도 -D 없음)
setlocal
set "APP=%~dp0"
cd /d "%APP%"
if "%ZEPHYR_BASE%"=="" (echo ZEPHYR_BASE not set & exit /b 1)
west build -b frdm_mcxn947/mcxn947/cpu0 . -p -d "%~dp0..\build-smartgateway"
endlocal
