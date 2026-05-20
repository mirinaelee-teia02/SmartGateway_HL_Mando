# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# boards/frdm_mcxn947_mcxn947_cpu0.overlay — Zephyr 자동 인식 (별도 지정 불필요)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(SmartGateway)

target_sources(app PRIVATE src/main.c src/adc.c src/udp.c src/msgpack_adc.c src/time_helper.c src/rs232.c src/tcp_gateway.c src/sync_gate.c src/gw_error.c)

target_sources(app PRIVATE src/config_nvs.c src/network_manager.c)

if(CONFIG_SMARTGATEWAY_WIFI_ENABLE)
  target_sources(app PRIVATE src/wifi_manager.c)
endif()

# NXP WiFi 미들웨어(mlan_sdio.c, wlan.c)가 fsl_gpio.h 를 include 하지만
# wifi_nxp/CMakeLists.txt 에 drivers/gpio 경로가 없음.
# MCXN947 GPIO = nxp,kinetis-gpio → drivers/gpio/fsl_gpio.h 를 전역으로 추가.
if(CONFIG_WIFI_NXP)
  zephyr_include_directories(
    ${ZEPHYR_HAL_NXP_MODULE_DIR}/mcux/mcux-sdk-ng/drivers/gpio
  )
endif()
