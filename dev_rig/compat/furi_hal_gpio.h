/*
 * Minimal GPIO shim. The dev rig drives the FUSB302 by polling and passes a
 * NULL interrupt pin to fusb302_init(), so these are only here to satisfy the
 * driver's declarations — they are never called at runtime. Wire INT to a GPIO
 * and flesh these out if you switch to interrupt-driven operation.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t pin;
} GpioPin;

typedef enum {
    GpioModeInput,
    GpioModeOutputPushPull,
} GpioMode;

typedef enum {
    GpioPullNo,
    GpioPullUp,
    GpioPullDown,
} GpioPull;

typedef enum {
    GpioSpeedLow,
    GpioSpeedHigh,
} GpioSpeed;

typedef enum {
    GpioAltFnUnused,
} GpioAltFn;

typedef enum {
    GpioConditionRise,
    GpioConditionFall,
} GpioCondition;

typedef void (*GpioInterruptCallback)(void* context);

void furi_hal_gpio_init_simple(const GpioPin* pin, GpioMode mode);
void furi_hal_gpio_init_ex(
    const GpioPin* pin,
    GpioMode mode,
    GpioPull pull,
    GpioSpeed speed,
    GpioAltFn alt_fn);
void furi_hal_gpio_add_int_callback(
    const GpioPin* pin,
    GpioCondition condition,
    GpioInterruptCallback callback,
    void* context);
void furi_hal_gpio_remove_int_callback(const GpioPin* pin);
