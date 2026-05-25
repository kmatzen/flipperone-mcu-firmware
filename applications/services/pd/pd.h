#pragma once
#include <furi.h>

#define RECORD_PD "pd"

typedef struct Pd Pd;

typedef enum {
    PdDeviceFusb302 = (1 << 0),
} PdDevice;

typedef enum {
    PdModeOff,
    PdModeDrp,
    PdModeSnk,
    PdModeSrc,
    PdModeCount,
} PdMode;

/** Events published on the pubsub returned by pd_get_pubsub(). */
typedef enum {
    PdEventTypeSourceAttached, //!< A PD source was detected; negotiation started
    PdEventTypeContract, //!< A power contract is in effect (voltage/current valid)
    PdEventTypeDetached, //!< Source detached or the contract was lost
    PdEventTypeFailed, //!< Negotiation failed (request rejected / hard reset)
} PdEventType;

typedef struct {
    PdEventType type;
    uint16_t voltage_mv; //!< Negotiated voltage, mV (valid for PdEventTypeContract)
    uint16_t current_ma; //!< Negotiated current, mA (valid for PdEventTypeContract)
} PdEvent;

#ifdef __cplusplus
extern "C" {
#endif
bool pd_is_device_initialized(Pd* instance, PdDevice* device);
FuriPubSub* pd_get_pubsub(Pd* pd);
bool pd_reset_config(Pd* instance);
bool pd_set_mode(Pd* instance, PdMode mode);
bool pd_get_mode(Pd* instance, PdMode* mode);

/**
 * Set the maximum voltage (mV) the sink is allowed to request. Applied the next
 * time sink mode starts negotiating. Must not exceed the board's VBUS rating.
 */
bool pd_set_sink_max_voltage(Pd* instance, uint16_t voltage_mv);

/**
 * Read the active power contract.
 * @return true if a contract is in effect; voltage_mv/current_ma are then filled.
 */
bool pd_get_contract(Pd* instance, uint16_t* voltage_mv, uint16_t* current_ma);

#ifdef __cplusplus
}
#endif
