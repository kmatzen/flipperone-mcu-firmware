#include "furi_hal_gpio.h"

/*
 * Stubs only: the rig passes a NULL interrupt pin to fusb302_init() and polls,
 * so none of these run. They exist so the driver links.
 */

void furi_hal_gpio_init_simple(const GpioPin* pin, GpioMode mode) {
    (void)pin;
    (void)mode;
}

void furi_hal_gpio_init_ex(
    const GpioPin* pin,
    GpioMode mode,
    GpioPull pull,
    GpioSpeed speed,
    GpioAltFn alt_fn) {
    (void)pin;
    (void)mode;
    (void)pull;
    (void)speed;
    (void)alt_fn;
}

void furi_hal_gpio_add_int_callback(
    const GpioPin* pin,
    GpioCondition condition,
    GpioInterruptCallback callback,
    void* context) {
    (void)pin;
    (void)condition;
    (void)callback;
    (void)context;
}

void furi_hal_gpio_remove_int_callback(const GpioPin* pin) {
    (void)pin;
}
