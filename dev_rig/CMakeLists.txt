# Standalone USB-PD sink bring-up rig for a Raspberry Pi Pico 2 (RP2350).
#
# Build (from the repo root):
#   export PICO_SDK_PATH=$PWD/pico-sdk            # any pico-sdk 2.x
#   export PATH=$PWD/toolchain/arm-none-eabi-gcc/bin:$PATH   # arm-none-eabi-gcc
#   cmake -S dev_rig -B dev_rig/build -G Ninja
#   cmake --build dev_rig/build
# Then flash dev_rig/build/pd_dev_rig.uf2 (hold BOOTSEL, plug Pico 2, drag/drop).

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(PICO_BOARD pico2 CACHE STRING "Board type")

# Reuse the firmware's SDK import (locates PICO_SDK_PATH).
include(${CMAKE_CURRENT_LIST_DIR}/../lib/sdk/pico_sdk_import.cmake)

project(pd_dev_rig C CXX ASM)

pico_sdk_init()

set(REPO_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)

add_executable(pd_dev_rig
    main.c
    compat/furi_hal_i2c.c
    compat/furi_hal_gpio.c
    # Real firmware code under test:
    ${REPO_ROOT}/lib/drivers/fusb302/fusb302.c
    ${REPO_ROOT}/applications/services/pd/pd_protocol.c
    ${REPO_ROOT}/applications/services/pd/pd_sink_policy.c
)

target_include_directories(pd_dev_rig PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/compat
    ${REPO_ROOT}/lib/drivers/fusb302
    ${REPO_ROOT}/applications/services/pd
)

target_link_libraries(pd_dev_rig
    pico_stdlib
    hardware_i2c
)

# CLI/log output over USB serial.
pico_enable_stdio_usb(pd_dev_rig 1)
pico_enable_stdio_uart(pd_dev_rig 0)

pico_add_extra_outputs(pd_dev_rig)
