/*
 * Standalone USB-PD sink bring-up rig for a Raspberry Pi Pico 2 (RP2350).
 *
 * Drives the project's real FUSB302 driver + sink policy against an external
 * FUSB302 breakout and prints the negotiated contract over USB serial. It is a
 * polled re-implementation of the `pd` service's interrupt handling — no furi
 * event loop, no FreeRTOS — so you can validate the driver and negotiation on
 * cheap hardware before the Flipper One MCU board exists.
 *
 * See README.md for wiring and build/flash instructions.
 */

#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/i2c.h"

#include "furi_hal_i2c_types.h"
#include "fusb302.h"
#include "pd_protocol.h"
#include "pd_sink_policy.h"

/* --- configuration ------------------------------------------------------- */
#define I2C_PORT i2c0
#define I2C_SDA_PIN 4
#define I2C_SCL_PIN 5
#define I2C_BAUD 400000

#define SINK_MAX_VOLTAGE_MV 9000 /* request up to this; keep within VBUS rating */
#define PD_SPEC_REV Fusb302SpecRev20
#define RX_DRAIN_LIMIT 8
#define POLL_INTERVAL_MS 5

static FuriHalI2cBusHandle i2c_handle;

/* --- glue: execute a policy decision ------------------------------------- */
static void execute_action(Fusb302* dev, const PdSinkAction* action) {
    if(action->send) {
        PdHeader header = {
            .message_type = action->tx_message_type,
            .data_role = PdDataRoleUfp,
            .spec_rev = PdSpecRev20,
            .power_role = PdPowerRoleSink,
            .message_id = action->tx_message_id,
            .num_objects = action->tx_object_count,
            .extended = false,
        };
        Fusb302PdMsg msg = {0};
        msg.sop_type = Fusb302PdSopTypeDefault;
        msg.header = pd_header_build(&header);
        msg.object_count = action->tx_object_count;
        for(uint8_t i = 0; i < action->tx_object_count; i++) {
            msg.objects[i] = action->tx_objects[i];
        }
        if(fusb302_pd_message_send(dev, &msg) != Fusb302StatusOk) {
            printf("[rig] send failed (type %u)\r\n", action->tx_message_type);
        }
    }

    switch(action->outcome) {
    case PdSinkOutcomeAttached:
        printf("[rig] source attached\r\n");
        break;
    case PdSinkOutcomeContract:
        printf(
            "[rig] CONTRACT: %u mV  %u mA\r\n",
            action->contract_voltage_mv,
            action->contract_current_ma);
        break;
    case PdSinkOutcomeDetached:
        printf("[rig] detached\r\n");
        break;
    case PdSinkOutcomeFailed:
        printf("[rig] negotiation failed (hard reset)\r\n");
        break;
    case PdSinkOutcomeNone:
    default:
        break;
    }
}

static void drain_rx(Fusb302* dev, PdSinkPolicy* policy) {
    for(int i = 0; i < RX_DRAIN_LIMIT; i++) {
        Fusb302PdMsg msg;
        Fusb302Status res = fusb302_pd_message_receive(dev, &msg);
        if(res == Fusb302StatusRxEmpty) {
            break;
        }
        if(res != Fusb302StatusOk) {
            printf("[rig] rx error\r\n");
            break;
        }
        if(msg.sop_type != Fusb302PdSopTypeDefault) {
            continue;
        }
        PdSinkAction action;
        pd_sink_policy_handle_message(policy, msg.header, msg.objects, msg.object_count, &action);
        execute_action(dev, &action);
    }
}

static void on_attach(Fusb302* dev, PdSinkPolicy* policy) {
    Fusb302TypeCcOrientation orientation;
    if(fusb302_detect_cc_orientation(dev, &orientation) != Fusb302StatusOk) {
        return;
    }
    if(orientation == Fusb302TypeCcOrientationNone) {
        printf("[rig] VBUS present but no CC detected\r\n");
        return;
    }
    if(fusb302_pd_sink_start(dev, orientation, PD_SPEC_REV) != Fusb302StatusOk) {
        return;
    }
    printf("[rig] attach on CC%d\r\n", orientation == Fusb302TypeCcOrientationNormal ? 1 : 2);

    PdSinkAction action;
    pd_sink_policy_attach(policy, &action);
    execute_action(dev, &action);
}

static void on_detach(Fusb302* dev, PdSinkPolicy* policy) {
    PdSinkAction action;
    pd_sink_policy_detach(policy, &action);
    fusb302_sink_arm(dev); /* re-arm for the next attach */
    execute_action(dev, &action);
}

int main(void) {
    stdio_init_all();
    sleep_ms(2000); /* let USB-CDC enumerate before the first prints */
    printf("\r\n[rig] FUSB302 USB-PD sink bring-up rig\r\n");

    i2c_init(I2C_PORT, I2C_BAUD);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);
    i2c_handle.inst = I2C_PORT;

    Fusb302* dev = fusb302_init(&i2c_handle, FUSB302_ADDRESS, NULL);
    if(!dev) {
        printf("[rig] FUSB302 not found at 0x%02X — check wiring/pull-ups\r\n", FUSB302_ADDRESS);
        for(;;) {
            tight_loop_contents();
        }
    }

    PdSinkPolicy policy;
    pd_sink_policy_init(&policy, SINK_MAX_VOLTAGE_MV);
    fusb302_sink_arm(dev);
    printf("[rig] armed, ceiling %u mV — plug in a PD charger\r\n", SINK_MAX_VOLTAGE_MV);

    bool last_vbus = false;
    for(;;) {
        bool vbus = false;
        if(fusb302_get_vbus_ok(dev, &vbus) == Fusb302StatusOk) {
            if(vbus && !last_vbus) {
                on_attach(dev, &policy);
            } else if(!vbus && last_vbus) {
                on_detach(dev, &policy);
            }
            last_vbus = vbus;
        }

        Fusb302Interrupts irq;
        if(fusb302_read_interrupts(dev, &irq) == Fusb302StatusOk && irq.hard_reset) {
            printf("[rig] hard reset received\r\n");
            PdSinkAction action;
            pd_sink_policy_hard_reset(&policy, &action);
            execute_action(dev, &action);
        }

        drain_rx(dev, &policy);
        sleep_ms(POLL_INTERVAL_MS);
    }
}
