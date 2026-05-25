#pragma once

#include "hardware/i2c.h"

/** Bus handle the fusb302 driver carries around; here it just wraps a pico i2c. */
typedef struct {
    i2c_inst_t* inst;
} FuriHalI2cBusHandle;
