#include "pd.h"
#include "pd_protocol.h"

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
/** Sink input current limit (matches the BQ25792 input current limit). */
#define PD_SINK_MAX_CURRENT_MA (3000)
/** Upper bound on PD messages drained per interrupt to bound loop time. */
#define PD_RX_DRAIN_LIMIT (8)
/** USB-PD revision used for headers and the chip's auto GoodCRC role bits. */
#define PD_SPEC_REV (Fusb302SpecRev20)

typedef enum {
    PdLoopFlagIsr = (1 << 0),
    PdLoopFlagAll = (PdLoopFlagIsr),
} PdLoopFlag;

/** Sink policy engine state. */
typedef enum {
    PdSinkStateIdle, //!< PD disabled
    PdSinkStateWaitVbus, //!< armed, waiting for a source to apply VBUS
    PdSinkStateWaitCaps, //!< configured, waiting for Source_Capabilities
    PdSinkStateRequested, //!< Request sent, waiting for Accept
    PdSinkStateWaitPsRdy, //!< Accept received, waiting for PS_RDY
    PdSinkStateReady, //!< power contract in effect
} PdSinkState;

struct Pd {
    FuriEventLoop* event_loop;
    FuriPubSub* event_pubsub;
    Fusb302* fusb302_header;
    PdMode mode;
    FuriMessageQueue* message_queue;
    PdDevice device;

    // sink policy engine
    PdSinkState sink_state;
    Fusb302TypeCcOrientation orientation;
    uint8_t tx_msg_id;
    uint16_t max_voltage_mv;

    // selection pending acceptance
    uint8_t selected_position;
    uint16_t selected_voltage_mv;
    uint16_t selected_current_ma;

    // active contract
    uint16_t contract_voltage_mv;
    uint16_t contract_current_ma;
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
/* Event publishing                                                    */
/* ------------------------------------------------------------------ */

static void pd_publish(Pd* instance, PdEventType type, uint16_t voltage_mv, uint16_t current_ma) {
    PdEvent event = {
        .type = type,
        .voltage_mv = voltage_mv,
        .current_ma = current_ma,
    };
    furi_pubsub_publish(instance->event_pubsub, &event);
}

/* ------------------------------------------------------------------ */
/* Sink message transmission                                           */
/* ------------------------------------------------------------------ */

static void
    pd_sink_send(Pd* instance, uint8_t message_type, const uint32_t* objects, uint8_t count) {
    PdHeader header = {
        .message_type = message_type,
        .data_role = PdDataRoleUfp,
        .spec_rev = PdSpecRev20,
        .power_role = PdPowerRoleSink,
        .message_id = instance->tx_msg_id,
        .num_objects = count,
        .extended = false,
    };

    Fusb302PdMsg msg = {0};
    msg.sop_type = Fusb302PdSopTypeDefault;
    msg.header = pd_header_build(&header);
    msg.object_count = count;
    for(uint8_t i = 0; i < count; i++) {
        msg.objects[i] = objects[i];
    }

    if(fusb302_pd_message_send(instance->fusb302_header, &msg) == Fusb302StatusOk) {
        instance->tx_msg_id = (instance->tx_msg_id + 1) & 0x07;
    } else {
        FURI_LOG_E(TAG, "Failed to send PD message type %u", message_type);
    }
}

static void pd_sink_send_control(Pd* instance, uint8_t control_type) {
    pd_sink_send(instance, control_type, NULL, 0);
}

static void pd_sink_send_sink_caps(Pd* instance) {
    /* Advertise a single vSafe5V Fixed PDO sized to our input current limit. */
    uint32_t pdo = pd_pdo_build_fixed(5000, PD_SINK_MAX_CURRENT_MA);
    pd_sink_send(instance, PdDataSinkCapabilities, &pdo, 1);
}

/* ------------------------------------------------------------------ */
/* Sink policy                                                         */
/* ------------------------------------------------------------------ */

static void pd_sink_handle_source_caps(Pd* instance, const Fusb302PdMsg* msg) {
    PdPdo chosen;
    uint8_t position =
        pd_select_fixed_pdo(msg->objects, msg->object_count, instance->max_voltage_mv, &chosen);

    uint32_t rdo;
    if(position == 0) {
        /* Nothing within our voltage ceiling: fall back to PDO #1 (always
         * vSafe5V) and flag a capability mismatch so the source knows. */
        PdPdo first;
        pd_pdo_parse(msg->objects[0], &first);
        uint16_t current = first.max_current_ma ? first.max_current_ma : 500;
        rdo = pd_rdo_build_fixed(1, current, current, true, true);
        instance->selected_position = 1;
        instance->selected_voltage_mv = first.voltage_mv;
        instance->selected_current_ma = current;
        FURI_LOG_W(
            TAG, "No PDO within %u mV; requesting 5V with mismatch", instance->max_voltage_mv);
    } else {
        uint16_t current = chosen.max_current_ma;
        if(current > PD_SINK_MAX_CURRENT_MA) {
            current = PD_SINK_MAX_CURRENT_MA;
        }
        rdo = pd_rdo_build_fixed(position, current, current, false, true);
        instance->selected_position = position;
        instance->selected_voltage_mv = chosen.voltage_mv;
        instance->selected_current_ma = current;
        FURI_LOG_I(
            TAG, "Selected PDO #%u: %u mV %u mA", position, chosen.voltage_mv, current);
    }

    pd_sink_send(instance, PdDataRequest, &rdo, 1);
    instance->sink_state = PdSinkStateRequested;
}

static void pd_sink_dispatch(Pd* instance, const Fusb302PdMsg* msg) {
    /* Only Source/Sink SOP traffic is relevant to the port partner contract. */
    if(msg->sop_type != Fusb302PdSopTypeDefault) {
        return;
    }

    PdHeader header;
    pd_header_parse(msg->header, &header);

    if(pd_header_is_control(msg->header)) {
        switch(header.message_type) {
        case PdControlAccept:
            if(instance->sink_state == PdSinkStateRequested) {
                instance->sink_state = PdSinkStateWaitPsRdy;
            }
            break;
        case PdControlPsRdy:
            if(instance->sink_state == PdSinkStateWaitPsRdy) {
                instance->sink_state = PdSinkStateReady;
                instance->contract_voltage_mv = instance->selected_voltage_mv;
                instance->contract_current_ma = instance->selected_current_ma;
                FURI_LOG_I(
                    TAG,
                    "PD contract: %u mV %u mA",
                    instance->contract_voltage_mv,
                    instance->contract_current_ma);
                pd_publish(
                    instance,
                    PdEventTypeContract,
                    instance->contract_voltage_mv,
                    instance->contract_current_ma);
            }
            break;
        case PdControlReject:
        case PdControlWait:
            FURI_LOG_W(
                TAG,
                "Request %s",
                header.message_type == PdControlReject ? "rejected" : "deferred");
            if(instance->sink_state == PdSinkStateRequested) {
                /* Keep waiting; the source may resend its capabilities. */
                instance->sink_state = PdSinkStateWaitCaps;
            }
            break;
        case PdControlSoftReset:
            /* Reset our MessageID counter and accept, per section 6.8.2. */
            instance->tx_msg_id = 0;
            pd_sink_send_control(instance, PdControlAccept);
            instance->sink_state = PdSinkStateWaitCaps;
            break;
        case PdControlGetSinkCap:
            pd_sink_send_sink_caps(instance);
            break;
        default:
            break;
        }
    } else {
        if(header.message_type == PdDataSourceCapabilities && msg->object_count > 0) {
            pd_sink_handle_source_caps(instance, msg);
        }
    }
}

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
        pd_sink_dispatch(instance, &msg);
    }
}

static void pd_sink_on_attach(Pd* instance) {
    Fusb302TypeCcOrientation orientation;
    if(fusb302_detect_cc_orientation(instance->fusb302_header, &orientation) != Fusb302StatusOk) {
        return;
    }
    if(orientation == Fusb302TypeCcOrientationNone) {
        instance->sink_state = PdSinkStateWaitVbus;
        return;
    }

    instance->orientation = orientation;
    instance->tx_msg_id = 0;
    if(fusb302_pd_sink_start(instance->fusb302_header, orientation, PD_SPEC_REV) !=
       Fusb302StatusOk) {
        return;
    }
    instance->sink_state = PdSinkStateWaitCaps;
    FURI_LOG_I(
        TAG,
        "Source attached on CC%d",
        orientation == Fusb302TypeCcOrientationNormal ? 1 : 2);
    pd_publish(instance, PdEventTypeSourceAttached, 0, 0);
}

static void pd_sink_on_detach(Pd* instance) {
    bool had_contract = (instance->sink_state == PdSinkStateReady);
    instance->sink_state = PdSinkStateWaitVbus;
    instance->tx_msg_id = 0;
    instance->contract_voltage_mv = 0;
    instance->contract_current_ma = 0;
    /* Re-arm so the next attach raises VBUSOK again. */
    fusb302_sink_arm(instance->fusb302_header);
    FURI_LOG_I(TAG, "Source detached");
    if(had_contract) {
        pd_publish(instance, PdEventTypeDetached, 0, 0);
    }
}

static void pd_handle_isr(Pd* instance) {
    Fusb302Interrupts irq;
    if(fusb302_read_interrupts(instance->fusb302_header, &irq) != Fusb302StatusOk) {
        return;
    }

    if(irq.hard_reset) {
        FURI_LOG_W(TAG, "PD hard reset received");
        bool had_contract = (instance->sink_state == PdSinkStateReady);
        instance->tx_msg_id = 0;
        instance->contract_voltage_mv = 0;
        instance->contract_current_ma = 0;
        instance->sink_state = PdSinkStateWaitCaps;
        if(had_contract) {
            pd_publish(instance, PdEventTypeFailed, 0, 0);
        }
    }

    if(irq.vbus_ok) {
        bool vbus = false;
        fusb302_get_vbus_ok(instance->fusb302_header, &vbus);
        if(vbus) {
            if(instance->sink_state == PdSinkStateWaitVbus) {
                pd_sink_on_attach(instance);
            }
        } else {
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
        instance->sink_state = PdSinkStateIdle;
        instance->contract_voltage_mv = 0;
        instance->contract_current_ma = 0;
        instance->mode = mode;
        return true;
    case PdModeSnk: {
        instance->mode = mode;
        instance->tx_msg_id = 0;
        instance->contract_voltage_mv = 0;
        instance->contract_current_ma = 0;
        if(fusb302_sink_arm(instance->fusb302_header) != Fusb302StatusOk) {
            return false;
        }
        instance->sink_state = PdSinkStateWaitVbus;
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
        instance->sink_state = PdSinkStateIdle;
        instance->contract_voltage_mv = 0;
        instance->contract_current_ma = 0;
        break;
    case PdMessageTypeSetMaxVoltage:
        instance->max_voltage_mv = msg.set_max_voltage;
        result = true;
        break;
    case PdMessageTypeGetContract: {
        bool active =
            (instance->sink_state == PdSinkStateReady) && (instance->contract_voltage_mv > 0);
        if(active) {
            if(msg.get_contract.voltage_mv) {
                *msg.get_contract.voltage_mv = instance->contract_voltage_mv;
            }
            if(msg.get_contract.current_ma) {
                *msg.get_contract.current_ma = instance->contract_current_ma;
            }
        }
        result = active;
        break;
    }
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
    instance->sink_state = PdSinkStateIdle;
    instance->orientation = Fusb302TypeCcOrientationNone;
    instance->tx_msg_id = 0;
    instance->max_voltage_mv = PD_SINK_DEFAULT_MAX_VOLTAGE_MV;
    instance->selected_position = 0;
    instance->selected_voltage_mv = 0;
    instance->selected_current_ma = 0;
    instance->contract_voltage_mv = 0;
    instance->contract_current_ma = 0;

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
