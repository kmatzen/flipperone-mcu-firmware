/*
 * Minimal furi shim for the standalone Pico 2 dev rig.
 *
 * Provides just enough of the furi surface for lib/drivers/fusb302/fusb302.c to
 * compile and run on a bare pico-sdk project. Logging maps to printf (USB
 * serial); furi_check halts so failures are visible on the console.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>

#include "pico/stdlib.h"

#ifndef UNUSED
#define UNUSED(x) ((void)(x))
#endif

#ifndef __isr
#define __isr
#endif

#ifndef __not_in_flash_func
#define __not_in_flash_func(func) func
#endif

#define FURI_LOG_E(tag, ...)         \
    do {                             \
        printf("[E][%s] ", tag);     \
        printf(__VA_ARGS__);         \
        printf("\r\n");              \
    } while(0)
#define FURI_LOG_W(tag, ...)         \
    do {                             \
        printf("[W][%s] ", tag);     \
        printf(__VA_ARGS__);         \
        printf("\r\n");              \
    } while(0)
#define FURI_LOG_I(tag, ...)         \
    do {                             \
        printf("[I][%s] ", tag);     \
        printf(__VA_ARGS__);         \
        printf("\r\n");              \
    } while(0)
#define FURI_LOG_D(tag, ...)

#define furi_check(cond, ...)                                                  \
    do {                                                                       \
        if(!(cond)) {                                                          \
            printf("furi_check failed: %s (%s:%d)\r\n", #cond, __FILE__, __LINE__); \
            for(;;) {                                                          \
                tight_loop_contents();                                         \
            }                                                                  \
        }                                                                      \
    } while(0)
#define furi_assert(cond, ...) furi_check(cond)

#define furi_crash(msg)                       \
    do {                                      \
        printf("furi_crash: %s\r\n", (msg));  \
        for(;;) {                             \
            tight_loop_contents();            \
        }                                     \
    } while(0)

static inline void furi_delay_ms(uint32_t ms) {
    sleep_ms(ms);
}

static inline void furi_log_puts(const char* s) {
    fputs(s, stdout);
}
