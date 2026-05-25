#include "furi_hal_i2c.h"

#include "hardware/i2c.h"
#include "pico/error.h"

/* The dev rig is single-threaded, so acquire/release are no-ops. */
void furi_hal_i2c_acquire(const FuriHalI2cBusHandle* handle) {
    (void)handle;
}

void furi_hal_i2c_release(const FuriHalI2cBusHandle* handle) {
    (void)handle;
}

bool furi_hal_i2c_device_ready(
    const FuriHalI2cBusHandle* handle,
    uint8_t address,
    uint32_t timeout_us) {
    uint8_t dummy = 0;
    int ret = i2c_read_timeout_us(handle->inst, address, &dummy, 1, false, timeout_us);
    return ret >= 0;
}

int furi_hal_i2c_master_tx_blocking(
    const FuriHalI2cBusHandle* handle,
    uint8_t address,
    const uint8_t* data,
    size_t size,
    uint32_t timeout_us) {
    return i2c_write_timeout_us(handle->inst, address, data, size, false, timeout_us);
}

int furi_hal_i2c_master_trx_blocking(
    const FuriHalI2cBusHandle* handle,
    uint8_t address,
    const uint8_t* tx_data,
    size_t tx_size,
    uint8_t* rx_data,
    size_t rx_size,
    uint32_t timeout_us) {
    /* Repeated start: keep the bus (nostop=true) between write and read. */
    int written = i2c_write_timeout_us(handle->inst, address, tx_data, tx_size, true, timeout_us);
    if(written < 0) {
        return written;
    }
    return i2c_read_timeout_us(handle->inst, address, rx_data, rx_size, false, timeout_us);
}
