#include "pd.h"
#include "pd_protocol.h"
#include "pd_sink_policy.h"

#include <furi.h>
#include <api_lock.h>
#include <furi_hal_i2c_config.h>
#include <furi_hal_resources.h>
#include <drivers/fusb302/fusb302.h>
#include <furi_bsp.h>

#define TAG "Pd"

#define PD_MAX_MESSAGES (8)
/** Safety ceiling for negotiated voltage. Must not exceed the board VBUS rating. */
#define PD_SINK_DEFAULT_MAX_VOLTAGE_MV (9000)
/** Upper bound on PD messages drained per interrupt to bound loop time. */
#define PD_RX_DRAIN_LIMIT (8)
/** USB-PD revision used for headers and the chip's auto GoodCRC role bits. */
#define PD_SPEC_REV (Fusb302SpecRev20)

typedef enum {
    PdLoopFlagIsr = (1 << 0),
    PdLoopFlagAll = (PdLoopFlagIsr),
} PdLoopFlag;

struct Pd {
    FuriEventLoop* event_loop;
    FuriPubSub* event_pubsub;
    Fusb302* fusb302_header;
    PdMode mode;
    FuriMessageQueue* message_queue;
    PdDevice device;
    PdSinkPolicy policy;
};

typedef enum {
    PdMessageTypeSetMode,
    PdMessageTypeGetMode,
    PdMessageTypeResetConfig,
    PdMessageTypeSetMaxVoltage,
    PdMessageTypeGetContract,
} PdMessageType;

typedef struct {
    PdMessageType type;
    FuriApiLock lock;
    bool* result;
    union {
        PdMode* get_mode;
        PdMode set_mode;
        uint16_t set_max_voltage;
        struct {
            uint16_t* voltage_mv;
            uint16_t* current_ma;
        } get_contract;
    };
} PdMessage;

/* ------------------------------------------------------------------ */
/* Action execution: turn policy decisions into hardware I/O / events  */
/* ------------------------------------------------------------------ */

static void pd_publish(Pd* instance, PdEventType type, uint16_t voltage_mv, uint16_t current_ma) {
    PdEvent event = {
        .type = type,
        .voltage_mv = voltage_mv,
        .current_ma = current_ma,
    };
    furi_pubsub_publish(instance->event_pubsub, &event);
}

static void pd_execute_action(Pd* instance, const PdSinkAction* action) {
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

        if(fusb302_pd_message_send(instance->fusb302_header, &msg) != Fusb302StatusOk) {
            FURI_LOG_E(TAG, "Failed to send PD message type %u", action->tx_message_type);
        }
    }

    switch(action->outcome) {
    case PdSinkOutcomeAttached:
        pd_publish(instance, PdEventTypeSourceAttached, 0, 0);
        break;
    case PdSinkOutcomeContract:
        FURI_LOG_I(
            TAG,
            "PD contract: %u mV %u mA",
            action->contract_voltage_mv,
            action->contract_current_ma);
        pd_publish(
            instance,
            PdEventTypeContract,
            action->contract_voltage_mv,
            action->contract_current_ma);
        break;
    case PdSinkOutcomeDetached:
        pd_publish(instance, PdEventTypeDetached, 0, 0);
        break;
    case PdSinkOutcomeFailed:
        pd_publish(instance, PdEventTypeFailed, 0, 0);
        break;
    case PdSinkOutcomeNone:
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Hardware glue around the policy                                     */
/* ------------------------------------------------------------------ */

static void pd_sink_drain_rx(Pd* instance) {
    for(uint8_t i = 0; i < PD_RX_DRAIN_LIMIT; i++) {
        Fusb302PdMsg msg;
        Fusb302Status res = fusb302_pd_message_receive(instance->fusb302_header, &msg);
        if(res == Fusb302StatusRxEmpty) {
            break;
        }
        if(res != Fusb302StatusOk) {
            FURI_LOG_W(TAG, "PD receive error");
            break;
        }
        /* Only the port-partner (SOP) contract is handled here. */
        if(msg.sop_type != Fusb302PdSopTypeDefault) {
            continue;
        }
        PdSinkAction action;
        pd_sink_policy_handle_message(
            &instance->policy, msg.header, msg.objects, msg.object_count, &action);
        pd_execute_action(instance, &action);
    }
}

static void pd_sink_on_attach(Pd* instance) {
    Fusb302TypeCcOrientation orientation;
    if(fusb302_detect_cc_orientation(instance->fusb302_header, &orientation) != Fusb302StatusOk) {
        return;
    }
    if(orientation == Fusb302TypeCcOrientationNone) {
        return;
    }
    if(fusb302_pd_sink_start(instance->fusb302_header, orientation, PD_SPEC_REV) !=
       Fusb302StatusOk) {
        return;
    }
    FURI_LOG_I(
        TAG, "Source attached on CC%d", orientation == Fusb302TypeCcOrientationNormal ? 1 : 2);

    PdSinkAction action;
    pd_sink_policy_attach(&instance->policy, &action);
    pd_execute_action(instance, &action);
}

static void pd_sink_on_detach(Pd* instance) {
    PdSinkAction action;
    pd_sink_policy_detach(&instance->policy, &action);
    /* Re-arm so the next attach raises VBUSOK again. */
    fusb302_sink_arm(instance->fusb302_header);
    FURI_LOG_I(TAG, "Source detached");
    pd_execute_action(instance, &action);
}

static void pd_handle_isr(Pd* instance) {
    Fusb302Interrupts irq;
    if(fusb302_read_interrupts(instance->fusb302_header, &irq) != Fusb302StatusOk) {
        return;
    }

    if(irq.hard_reset) {
        FURI_LOG_W(TAG, "PD hard reset received");
        PdSinkAction action;
        pd_sink_policy_hard_reset(&instance->policy, &action);
        pd_execute_action(instance, &action);
    }

    if(irq.vbus_ok) {
        bool vbus = false;
        fusb302_get_vbus_ok(instance->fusb302_header, &vbus);
        if(vbus) {
            if(instance->mode == PdModeSnk &&
               pd_sink_policy_state(&instance->policy) == PdSinkStateIdle) {
                pd_sink_on_attach(instance);
            }
        } else if(instance->mode == PdModeSnk) {
            pd_sink_on_detach(instance);
        }
    }

    if(irq.good_crc_sent) {
        pd_sink_drain_rx(instance);
    }

    if(irq.retry_fail) {
        FURI_LOG_W(TAG, "PD transmit retries exhausted");
    }
}

static bool pd_apply_mode(Pd* instance, PdMode mode) {
    switch(mode) {
    case PdModeOff:
        fusb302_pd_disable(instance->fusb302_header);
        pd_sink_policy_reset(&instance->policy);
        instance->mode = mode;
        return true;
    case PdModeSnk: {
        instance->mode = mode;
        pd_sink_policy_reset(&instance->policy);
        if(fusb302_sink_arm(instance->fusb302_header) != Fusb302StatusOk) {
            return false;
        }
        /* If a source is already attached, begin negotiating immediately. */
        bool vbus = false;
        fusb302_get_vbus_ok(instance->fusb302_header, &vbus);
        if(vbus) {
            pd_sink_on_attach(instance);
        }
        return true;
    }
    case PdModeDrp:
    case PdModeSrc:
    default:
        FURI_LOG_W(TAG, "PD mode %d not supported yet", mode);
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* Event loop plumbing                                                 */
/* ------------------------------------------------------------------ */

static void __isr __not_in_flash_func(pd_event_isr)(void* context) {
    Pd* instance = (Pd*)context;
    furi_event_loop_set_custom_event(instance->event_loop, PdLoopFlagIsr);
}

static void pd_message_queue_callback(FuriEventLoopObject* object, void* context) {
    furi_assert(context);
    Pd* instance = context;
    furi_assert(object == instance->message_queue);

    PdMessage msg;
    furi_check(furi_message_queue_get(instance->message_queue, &msg, 0) == FuriStatusOk);

    bool result = false;

    switch(msg.type) {
    case PdMessageTypeSetMode:
        result = pd_apply_mode(instance, msg.set_mode);
        break;
    case PdMessageTypeGetMode:
        *(msg.get_mode) = instance->mode;
        result = true;
        break;
    case PdMessageTypeResetConfig:
        result = fusb302_sw_reset(instance->fusb302_header) == Fusb302StatusOk;
        pd_sink_policy_reset(&instance->policy);
        break;
    case PdMessageTypeSetMaxVoltage:
        pd_sink_policy_set_max_voltage(&instance->policy, msg.set_max_voltage);
        result = true;
        break;
    case PdMessageTypeGetContract:
        result = pd_sink_policy_contract(
            &instance->policy, msg.get_contract.voltage_mv, msg.get_contract.current_ma);
        break;
    default:
        furi_crash("Invalid message type");
        break;
    }

    if(msg.result) {
        *msg.result = result;
    }

    if(msg.lock) {
        api_lock_unlock(msg.lock);
    }
}

static void pd_custom_event_callback(uint32_t events, void* context) {
    furi_assert(context);
    Pd* instance = (Pd*)context;

    if(events & PdLoopFlagIsr) {
        pd_handle_isr(instance);
    }
}

static void pd_send_message(Pd* instance, const PdMessage* message) {
    furi_check(
        furi_message_queue_put(instance->message_queue, message, FuriWaitForever) ==
        FuriStatusOk);

    if(message->lock) {
        api_lock_wait_unlock_and_free(message->lock);
    }
}

static Pd* pd_alloc(void) {
    Pd* instance = (Pd*)malloc(sizeof(Pd));
    instance->event_loop = furi_event_loop_alloc();
    instance->message_queue = furi_message_queue_alloc(PD_MAX_MESSAGES, sizeof(PdMessage));
    instance->fusb302_header = fusb302_init(&furi_hal_i2c_handle_main, FUSB302_ADDRESS, NULL);
    instance->mode = PdModeOff;
    instance->device = 0;
    pd_sink_policy_init(&instance->policy, PD_SINK_DEFAULT_MAX_VOLTAGE_MV);

    if(instance->fusb302_header) {
        instance->device |= PdDeviceFusb302;
        furi_bsp_expander_main_attach_fusb302_callback(pd_event_isr, instance);
    } else {
        FURI_LOG_E(TAG, "Failed to initialize FUSB302");
    }

    furi_event_loop_subscribe_message_queue(
        instance->event_loop,
        instance->message_queue,
        FuriEventLoopEventIn,
        pd_message_queue_callback,
        instance);
    furi_event_loop_set_custom_event_callback(
        instance->event_loop, pd_custom_event_callback, instance);

    instance->event_pubsub = furi_pubsub_alloc();
    furi_record_create(RECORD_PD, instance);

    return instance;
}

bool pd_is_device_initialized(Pd* instance, PdDevice* device) {
    furi_check(instance);
    bool initialized = (instance->device & PdDeviceFusb302) == PdDeviceFusb302;

    if(device) {
        *device = instance->device;
    }
    if(!initialized) {
        FURI_LOG_E(TAG, "PD device not initialized");
    }
    return initialized;
}

int32_t pd_srv(void* p) {
    UNUSED(p);

    Pd* instance = pd_alloc();
    furi_event_loop_run(instance->event_loop);

    return 0;
}

bool pd_set_mode(Pd* instance, PdMode mode) {
    furi_check(instance);
    furi_check(mode < PdModeCount);
    if(pd_is_device_initialized(instance, NULL)) {
        bool result = false;
        PdMessage msg = {
            .type = PdMessageTypeSetMode,
            .set_mode = mode,
            .result = &result,
            .lock = api_lock_alloc_locked(),
        };

        pd_send_message(instance, &msg);
        return result;
    }
    return false;
}

bool pd_get_mode(Pd* instance, PdMode* mode) {
    furi_check(instance);
    if(pd_is_device_initialized(instance, NULL)) {
        PdMessage msg = {
            .type = PdMessageTypeGetMode,
            .get_mode = mode,
            .lock = api_lock_alloc_locked(),
        };

        pd_send_message(instance, &msg);
        return true;
    }
    return false;
}

bool pd_set_sink_max_voltage(Pd* instance, uint16_t voltage_mv) {
    furi_check(instance);
    if(pd_is_device_initialized(instance, NULL)) {
        bool result = false;
        PdMessage msg = {
            .type = PdMessageTypeSetMaxVoltage,
            .set_max_voltage = voltage_mv,
            .result = &result,
            .lock = api_lock_alloc_locked(),
        };

        pd_send_message(instance, &msg);
        return result;
    }
    return false;
}

bool pd_get_contract(Pd* instance, uint16_t* voltage_mv, uint16_t* current_ma) {
    furi_check(instance);
    if(pd_is_device_initialized(instance, NULL)) {
        bool result = false;
        PdMessage msg = {
            .type = PdMessageTypeGetContract,
            .result = &result,
            .lock = api_lock_alloc_locked(),
            .get_contract = {.voltage_mv = voltage_mv, .current_ma = current_ma},
        };

        pd_send_message(instance, &msg);
        return result;
    }
    return false;
}

bool pd_reset_config(Pd* instance) {
    furi_check(instance);
    if(pd_is_device_initialized(instance, NULL)) {
        bool result = false;
        PdMessage msg = {
            .type = PdMessageTypeResetConfig,
            .result = &result,
            .lock = api_lock_alloc_locked(),
        };

        pd_send_message(instance, &msg);
        return result;
    }
    return false;
}

FuriPubSub* pd_get_pubsub(Pd* pd) {
    furi_check(pd);
    return pd->event_pubsub;
}
