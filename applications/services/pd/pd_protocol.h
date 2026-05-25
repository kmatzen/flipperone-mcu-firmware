#pragma once

/**
 * USB Power Delivery protocol helpers.
 *
 * This module is intentionally free of any hardware or RTOS dependency: it only
 * deals with the bit layout of PD message headers, Power Data Objects (PDO) and
 * Request Data Objects (RDO) as defined by the USB Power Delivery Specification.
 * Keeping it self-contained makes the spec-critical packing/unpacking testable
 * on the host (see tests/pd_protocol_test.c).
 *
 * References: USB Power Delivery Specification Revision 3.1, Version 1.8,
 * sections 6.2 (Messages), 6.4.1 (Source Capabilities / PDO) and 6.4.2 (Request).
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of data objects a (non-extended) PD message can carry. */
#define PD_MAX_OBJECTS 7

/** Specification Revision field of the message header (bits 7:6). */
typedef enum {
    PdSpecRev10 = 0,
    PdSpecRev20 = 1,
    PdSpecRev30 = 2,
} PdSpecRev;

/** Port Power Role (header bit 8 for SOP). */
typedef enum {
    PdPowerRoleSink = 0,
    PdPowerRoleSource = 1,
} PdPowerRole;

/** Port Data Role (header bit 5 for SOP). */
typedef enum {
    PdDataRoleUfp = 0,
    PdDataRoleDfp = 1,
} PdDataRole;

/**
 * Control Message types (header Message Type field, when Number of Data Objects
 * == 0). @see Table 6-5.
 */
typedef enum {
    PdControlGoodCrc = 0x01,
    PdControlGotoMin = 0x02,
    PdControlAccept = 0x03,
    PdControlReject = 0x04,
    PdControlPing = 0x05,
    PdControlPsRdy = 0x06,
    PdControlGetSourceCap = 0x07,
    PdControlGetSinkCap = 0x08,
    PdControlDrSwap = 0x09,
    PdControlPrSwap = 0x0A,
    PdControlVconnSwap = 0x0B,
    PdControlWait = 0x0C,
    PdControlSoftReset = 0x0D,
    PdControlNotSupported = 0x10,
    PdControlGetSourceCapExtended = 0x11,
    PdControlGetStatus = 0x12,
    PdControlFrSwap = 0x13,
    PdControlGetPpsStatus = 0x14,
    PdControlGetCountryCodes = 0x15,
    PdControlGetSinkCapExtended = 0x16,
} PdControlMessage;

/**
 * Data Message types (header Message Type field, when Number of Data Objects
 * > 0). @see Table 6-6.
 */
typedef enum {
    PdDataSourceCapabilities = 0x01,
    PdDataRequest = 0x02,
    PdDataBist = 0x03,
    PdDataSinkCapabilities = 0x04,
    PdDataBatteryStatus = 0x05,
    PdDataAlert = 0x06,
    PdDataGetCountryInfo = 0x07,
    PdDataEnterUsb = 0x08,
    PdDataVendorDefined = 0x0F,
} PdDataMessage;

/** Parsed view of a 16-bit PD message header. */
typedef struct {
    uint8_t message_type; //!< Message Type field (bits 4:0)
    PdDataRole data_role; //!< Port Data Role (bit 5)
    PdSpecRev spec_rev; //!< Specification Revision (bits 7:6)
    PdPowerRole power_role; //!< Port Power Role (bit 8)
    uint8_t message_id; //!< MessageID (bits 11:9)
    uint8_t num_objects; //!< Number of Data Objects (bits 14:12)
    bool extended; //!< Extended message flag (bit 15)
} PdHeader;

/** Source PDO supply type (PDO bits 31:30). */
typedef enum {
    PdPdoFixed = 0,
    PdPdoBattery = 1,
    PdPdoVariable = 2,
    PdPdoAugmented = 3,
} PdPdoType;

/** Decoded Source Power Data Object. */
typedef struct {
    PdPdoType type;
    uint16_t voltage_mv; //!< Nominal (Fixed) or max (Battery/Variable/PPS) voltage, mV
    uint16_t min_voltage_mv; //!< Min voltage for Battery/Variable/PPS, mV (0 for Fixed)
    uint16_t max_current_ma; //!< Max current, mA (0 for Battery supplies)
    uint16_t max_power_mw; //!< Max power, mW (Battery supplies only, else 0)
    bool dual_role_power; //!< Fixed PDO only: partner is dual-role power
    bool usb_comms_capable; //!< Fixed PDO only: USB data communication capable
} PdPdo;

/* ------------------------------------------------------------------ */
/* Message header                                                      */
/* ------------------------------------------------------------------ */

/** Pack a PdHeader description into the 16-bit on-wire header value. */
uint16_t pd_header_build(const PdHeader* header);

/** Unpack a 16-bit on-wire header value into a PdHeader description. */
void pd_header_parse(uint16_t raw, PdHeader* header);

/** True if the header describes a control message (no data objects). */
bool pd_header_is_control(uint16_t raw);

/* ------------------------------------------------------------------ */
/* Power Data Objects                                                  */
/* ------------------------------------------------------------------ */

/** Decode a raw 32-bit Source PDO into a PdPdo. */
void pd_pdo_parse(uint32_t raw, PdPdo* pdo);

/** Encode a Fixed-supply PDO (used for advertising Sink_Capabilities). */
uint32_t pd_pdo_build_fixed(uint16_t voltage_mv, uint16_t max_current_ma);

/**
 * Build a Fixed/Variable Request Data Object.
 *
 * @param object_position 1-based index of the selected PDO in Source_Capabilities (1..7)
 * @param operating_ma    requested operating current in mA
 * @param max_ma          requested maximum operating current in mA
 * @param capability_mismatch set when no offered PDO satisfies the sink's needs
 * @param usb_comms       advertise USB communications capability
 * @return packed 32-bit RDO
 */
uint32_t pd_rdo_build_fixed(
    uint8_t object_position,
    uint16_t operating_ma,
    uint16_t max_ma,
    bool capability_mismatch,
    bool usb_comms);

/**
 * Select the best Fixed-supply PDO from a Source_Capabilities message.
 *
 * Picks the Fixed PDO with the highest voltage that does not exceed
 * @p max_voltage_mv. Among equal voltages the one offering more current wins.
 * Augmented/Battery/Variable PDOs are ignored.
 *
 * @param objects          array of raw 32-bit PDOs (as received)
 * @param count            number of PDOs in @p objects
 * @param max_voltage_mv   sink voltage ceiling in mV
 * @param[out] selected    decoded chosen PDO (may be NULL)
 * @return 1-based object position of the chosen PDO, or 0 if none is suitable
 */
uint8_t pd_select_fixed_pdo(
    const uint32_t* objects,
    size_t count,
    uint16_t max_voltage_mv,
    PdPdo* selected);

#ifdef __cplusplus
}
#endif
