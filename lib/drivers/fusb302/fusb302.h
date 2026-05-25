#pragma once

#include <furi_hal_i2c_types.h>
#include <furi_hal_gpio.h>

#define FUSB302_ADDRESS 0x22

typedef struct Fusb302 Fusb302;
typedef void (*Fusb302Callback)(void* context);

typedef enum {
    Fusb302StatusUnknown = 0,
    Fusb302StatusOk = 1,
    Fusb302StatusRxEmpty,
    Fusb302StatusTxEmpty,
    Fusb302StatusError = -1,
    Fusb302StatusTimeout = -2,
} Fusb302Status;

/**
 * List of possible destinations.
 * @see Table 6-4 Destination.
 */
typedef enum {
    Fusb302PdSopTypeDefault,
    Fusb302PdSopTypePrime,
    Fusb302PdSopTypePrimeDouble,
    Fusb302PdSopTypeDebug,
    Fusb302PdSopTypeDebugDouble,
    Fusb302PdSopTypeUnknown,
} Fusb302PdSopType;

typedef enum {
    Fusb302TypeCcOrientationNone, //!< No cable connected
    Fusb302TypeCcOrientationNormal, //!< Cable is plugged in normally (CC1 active)
    Fusb302TypeCcOrientationReverse, //!< Cable is plugged in upside-down (CC2 active)
} Fusb302TypeCcOrientation;

/**
 * Specification Revision used when the chip auto-generates GoodCRC packets.
 * Matches the Specification Revision field of the PD message header.
 */
typedef enum {
    Fusb302SpecRev10 = 0,
    Fusb302SpecRev20 = 1,
    Fusb302SpecRev30 = 2,
} Fusb302SpecRev;

/**
 * Decoded snapshot of the three FUSB302 interrupt registers.
 *
 * Reading the interrupt registers clears them, so a single call to
 * @ref fusb302_read_interrupts returns every event that fired since the last
 * read. @see registers INTERRUPT (0x42), INTERRUPTA (0x3E), INTERRUPTB (0x3F).
 */
typedef struct {
    bool vbus_ok; //!< VBUS crossed the valid threshold (I_VBUSOK)
    bool comp_changed; //!< Measure comparator output changed (I_COMP_CHNG)
    bool bc_level; //!< Requested current level changed (I_BC_LVL)
    bool collision; //!< Collision detected on transmit (I_COLLISION)
    bool crc_check; //!< Incoming packet CRC validity updated (I_CRC_CHK)
    bool activity; //!< CC bus activity changed (I_ACTIVITY)
    bool hard_reset; //!< Hard Reset ordered set received (I_HARDRST)
    bool soft_reset; //!< Soft Reset packet received (I_SOFTRST)
    bool tx_sent; //!< Our packet was acknowledged with GoodCRC (I_TXSENT)
    bool retry_fail; //!< Automatic retries exhausted (I_RETRYFAIL)
    bool toggle_done; //!< DRP/SNK/SRC toggle settled (I_TOGDONE)
    bool good_crc_sent; //!< We received a good packet and sent GoodCRC (I_GCRCSENT)
} Fusb302Interrupts;

/**
 * USB Power Delivery message structure.
 *
 * This structure represents a complete USB PD message including:
 * - SOP type (destination: device, cable plug prime, or cable plug double prime)
 * - Message header containing message type, data role, power role, etc.
 * - Optional data objects (up to 7 objects)
 *
 * @see Section 6.2 of the USB Power Delivery Specification
 */
typedef struct {
    Fusb302PdSopType sop_type;
    uint16_t header;
    uint32_t objects[7];
    uint8_t object_count;
} Fusb302PdMsg;

#ifdef __cplusplus
extern "C" {
#endif

Fusb302* fusb302_init(const FuriHalI2cBusHandle* i2c_handle, uint8_t address, const GpioPin* pin_interrupt);
void fusb302_read_cc_status(Fusb302* instance, uint8_t cc);
void fusb302_deinit(Fusb302* instance);
bool fusb302_read_role(Fusb302* instance);
void fusb302_set_input_callback(Fusb302* instance, Fusb302Callback callback, void* context);
Fusb302Status fusb302_start_drp_logic(Fusb302* instance);
Fusb302Status fusb302_sw_reset(Fusb302* instance);

Fusb302Status fusb302_cc_orientation_set(Fusb302* instance, Fusb302TypeCcOrientation orientation);

// sink
/** Read and clear all pending interrupts into @p out. */
Fusb302Status fusb302_read_interrupts(Fusb302* instance, Fusb302Interrupts* out);
/** Read the current VBUS-present status (Status0.VBUSOK). */
Fusb302Status fusb302_get_vbus_ok(Fusb302* instance, bool* vbus_ok);
/**
 * Detect the attached cable orientation while operating as a sink.
 *
 * Presents Rd on both CC lines and measures each in turn; the line pulled up by
 * the source's Rp reports the higher level. Returns
 * Fusb302TypeCcOrientationNone when neither line is driven.
 */
Fusb302Status
    fusb302_detect_cc_orientation(Fusb302* instance, Fusb302TypeCcOrientation* orientation);
/**
 * Bring the chip up as a USB-PD sink on @p orientation.
 *
 * Powers all blocks, presents Rd, routes BMC RX/TX to the active CC, programs
 * the GoodCRC role bits (sink / UFP / @p rev), enables auto GoodCRC and retries,
 * unmasks the sink-relevant interrupts and resets the PD logic. After this the
 * sink is ready to receive Source_Capabilities and transmit a Request.
 */
Fusb302Status fusb302_pd_sink_start(
    Fusb302* instance,
    Fusb302TypeCcOrientation orientation,
    Fusb302SpecRev rev);
/**
 * Arm sink attach detection.
 *
 * Presents Rd, powers the sense blocks and unmasks only the VBUSOK interrupt so
 * the INT line fires when a source applies VBUS. Call @ref fusb302_pd_sink_start
 * afterwards (once the orientation is known) to begin negotiating.
 */
Fusb302Status fusb302_sink_arm(Fusb302* instance);
/** Disable PD: mask interrupts and drop into low-power (after a soft reset). */
Fusb302Status fusb302_pd_disable(Fusb302* instance);

// pd
Fusb302Status fusb302_pd_reset_logic(Fusb302* instance);
Fusb302Status fusb302_pd_reset_hard(Fusb302* instance);
Fusb302Status fusb302_pd_autogoodcrc_set(Fusb302* instance, bool enabled);
Fusb302Status fusb302_pd_autoretry_set(Fusb302* instance, int retries);
Fusb302Status fusb302_pd_rx_flush(Fusb302* instance);
Fusb302Status fusb302_pd_tx_flush(Fusb302* instance);
Fusb302Status fusb302_pd_message_receive(Fusb302* instance, Fusb302PdMsg* msg);
Fusb302Status fusb302_pd_message_send(Fusb302* instance, Fusb302PdMsg* msg);

#ifdef __cplusplus
}
#endif
