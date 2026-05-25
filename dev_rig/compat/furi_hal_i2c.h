#pragma once

#include "furi_hal_i2c_types.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/** Per-transfer timeout used by the fusb302 driver. */
#define FURI_HAL_I2C_TIMEOUT_US (10000)

void furi_hal_i2c_acquire(const FuriHalI2cBusHandle* handle);
void furi_hal_i2c_release(const FuriHalI2cBusHandle* handle);

bool furi_hal_i2c_device_ready(
    const FuriHalI2cBusHandle* handle,
    uint8_t address,
    uint32_t timeout_us);

/** Returns bytes written, or a negative PICO_ERROR_* code. */
int furi_hal_i2c_master_tx_blocking(
    const FuriHalI2cBusHandle* handle,
    uint8_t address,
    const uint8_t* data,
    size_t size,
    uint32_t timeout_us);

/** Write then read with a repeated start. Returns bytes read, or PICO_ERROR_*. */
int furi_hal_i2c_master_trx_blocking(
    const FuriHalI2cBusHandle* handle,
    uint8_t address,
    const uint8_t* tx_data,
    size_t tx_size,
    uint8_t* rx_data,
    size_t rx_size,
    uint32_t timeout_us);
