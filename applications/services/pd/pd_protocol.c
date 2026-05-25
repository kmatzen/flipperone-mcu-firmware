#include "pd_protocol.h"

/* Message header field layout (USB PD Rev 3.1, section 6.2.1.1). */
#define PD_HDR_MSG_TYPE_SHIFT 0
#define PD_HDR_MSG_TYPE_MASK 0x1F
#define PD_HDR_DATA_ROLE_SHIFT 5
#define PD_HDR_SPEC_REV_SHIFT 6
#define PD_HDR_SPEC_REV_MASK 0x03
#define PD_HDR_POWER_ROLE_SHIFT 8
#define PD_HDR_MSG_ID_SHIFT 9
#define PD_HDR_MSG_ID_MASK 0x07
#define PD_HDR_NUM_OBJ_SHIFT 12
#define PD_HDR_NUM_OBJ_MASK 0x07
#define PD_HDR_EXTENDED_SHIFT 15

uint16_t pd_header_build(const PdHeader* header) {
    uint16_t raw = 0;
    raw |= ((uint16_t)(header->message_type & PD_HDR_MSG_TYPE_MASK)) << PD_HDR_MSG_TYPE_SHIFT;
    raw |= ((uint16_t)(header->data_role & 0x01)) << PD_HDR_DATA_ROLE_SHIFT;
    raw |= ((uint16_t)(header->spec_rev & PD_HDR_SPEC_REV_MASK)) << PD_HDR_SPEC_REV_SHIFT;
    raw |= ((uint16_t)(header->power_role & 0x01)) << PD_HDR_POWER_ROLE_SHIFT;
    raw |= ((uint16_t)(header->message_id & PD_HDR_MSG_ID_MASK)) << PD_HDR_MSG_ID_SHIFT;
    raw |= ((uint16_t)(header->num_objects & PD_HDR_NUM_OBJ_MASK)) << PD_HDR_NUM_OBJ_SHIFT;
    raw |= ((uint16_t)(header->extended ? 1 : 0)) << PD_HDR_EXTENDED_SHIFT;
    return raw;
}

void pd_header_parse(uint16_t raw, PdHeader* header) {
    header->message_type = (raw >> PD_HDR_MSG_TYPE_SHIFT) & PD_HDR_MSG_TYPE_MASK;
    header->data_role = (PdDataRole)((raw >> PD_HDR_DATA_ROLE_SHIFT) & 0x01);
    header->spec_rev = (PdSpecRev)((raw >> PD_HDR_SPEC_REV_SHIFT) & PD_HDR_SPEC_REV_MASK);
    header->power_role = (PdPowerRole)((raw >> PD_HDR_POWER_ROLE_SHIFT) & 0x01);
    header->message_id = (raw >> PD_HDR_MSG_ID_SHIFT) & PD_HDR_MSG_ID_MASK;
    header->num_objects = (raw >> PD_HDR_NUM_OBJ_SHIFT) & PD_HDR_NUM_OBJ_MASK;
    header->extended = ((raw >> PD_HDR_EXTENDED_SHIFT) & 0x01) != 0;
}

bool pd_header_is_control(uint16_t raw) {
    uint8_t num_objects = (raw >> PD_HDR_NUM_OBJ_SHIFT) & PD_HDR_NUM_OBJ_MASK;
    bool extended = ((raw >> PD_HDR_EXTENDED_SHIFT) & 0x01) != 0;
    return (num_objects == 0) && !extended;
}

/* PDO field scaling (USB PD Rev 3.1, section 6.4.1). */
#define PD_PDO_TYPE_SHIFT 30
#define PD_PDO_TYPE_MASK 0x03
#define PD_PDO_VOLT_50MV_MASK 0x3FF /* 10-bit fields scaled by 50 mV */
#define PD_PDO_CURR_10MA_MASK 0x3FF /* 10-bit fields scaled by 10 mA */
#define PD_PDO_PWR_250MW_MASK 0x3FF /* 10-bit field scaled by 250 mW */

/* Augmented PDO (PPS) sub-fields use different scaling. */
#define PD_APDO_TYPE_SHIFT 28
#define PD_APDO_TYPE_MASK 0x03
#define PD_APDO_MAX_VOLT_SHIFT 17 /* 8-bit, 100 mV */
#define PD_APDO_MIN_VOLT_SHIFT 8 /* 8-bit, 100 mV */
#define PD_APDO_MAX_CURR_SHIFT 0 /* 7-bit, 50 mA */
#define PD_APDO_VOLT_100MV_MASK 0xFF
#define PD_APDO_CURR_50MA_MASK 0x7F

void pd_pdo_parse(uint32_t raw, PdPdo* pdo) {
    pdo->type = (PdPdoType)((raw >> PD_PDO_TYPE_SHIFT) & PD_PDO_TYPE_MASK);
    pdo->voltage_mv = 0;
    pdo->min_voltage_mv = 0;
    pdo->max_current_ma = 0;
    pdo->max_power_mw = 0;
    pdo->dual_role_power = false;
    pdo->usb_comms_capable = false;

    switch(pdo->type) {
    case PdPdoFixed:
        /* bits 19:10 voltage (50 mV), bits 9:0 max current (10 mA) */
        pdo->voltage_mv = (uint16_t)(((raw >> 10) & PD_PDO_VOLT_50MV_MASK) * 50u);
        pdo->max_current_ma = (uint16_t)((raw & PD_PDO_CURR_10MA_MASK) * 10u);
        pdo->dual_role_power = ((raw >> 29) & 0x01) != 0;
        pdo->usb_comms_capable = ((raw >> 26) & 0x01) != 0;
        break;
    case PdPdoBattery:
        /* bits 29:20 max voltage, 19:10 min voltage (50 mV), 9:0 max power (250 mW) */
        pdo->voltage_mv = (uint16_t)(((raw >> 20) & PD_PDO_VOLT_50MV_MASK) * 50u);
        pdo->min_voltage_mv = (uint16_t)(((raw >> 10) & PD_PDO_VOLT_50MV_MASK) * 50u);
        pdo->max_power_mw = (uint16_t)((raw & PD_PDO_PWR_250MW_MASK) * 250u);
        break;
    case PdPdoVariable:
        /* bits 29:20 max voltage, 19:10 min voltage (50 mV), 9:0 max current (10 mA) */
        pdo->voltage_mv = (uint16_t)(((raw >> 20) & PD_PDO_VOLT_50MV_MASK) * 50u);
        pdo->min_voltage_mv = (uint16_t)(((raw >> 10) & PD_PDO_VOLT_50MV_MASK) * 50u);
        pdo->max_current_ma = (uint16_t)((raw & PD_PDO_CURR_10MA_MASK) * 10u);
        break;
    case PdPdoAugmented:
        /* Only SPR Programmable Power Supply (APDO type 0) is decoded. */
        pdo->voltage_mv =
            (uint16_t)(((raw >> PD_APDO_MAX_VOLT_SHIFT) & PD_APDO_VOLT_100MV_MASK) * 100u);
        pdo->min_voltage_mv =
            (uint16_t)(((raw >> PD_APDO_MIN_VOLT_SHIFT) & PD_APDO_VOLT_100MV_MASK) * 100u);
        pdo->max_current_ma =
            (uint16_t)(((raw >> PD_APDO_MAX_CURR_SHIFT) & PD_APDO_CURR_50MA_MASK) * 50u);
        break;
    }
}

uint32_t pd_pdo_build_fixed(uint16_t voltage_mv, uint16_t max_current_ma) {
    uint32_t volt_units = (uint32_t)(voltage_mv / 50u) & PD_PDO_VOLT_50MV_MASK;
    uint32_t curr_units = (uint32_t)(max_current_ma / 10u) & PD_PDO_CURR_10MA_MASK;
    /* Type field (bits 31:30) is 00 for a Fixed supply. */
    return (volt_units << 10) | curr_units;
}

/* Fixed/Variable Request Data Object layout (USB PD Rev 3.1, section 6.4.2). */
#define PD_RDO_OBJ_POS_SHIFT 28
#define PD_RDO_CAP_MISMATCH_SHIFT 26
#define PD_RDO_USB_COMMS_SHIFT 25
#define PD_RDO_NO_USB_SUSPEND_SHIFT 24
#define PD_RDO_OP_CURRENT_SHIFT 10
#define PD_RDO_MAX_CURRENT_SHIFT 0
#define PD_RDO_CURRENT_MASK 0x3FF

static uint16_t pd_ma_to_10ma(uint16_t ma) {
    /* Round up so we never request less than asked for. */
    return (uint16_t)((ma + 9u) / 10u);
}

uint32_t pd_rdo_build_fixed(
    uint8_t object_position,
    uint16_t operating_ma,
    uint16_t max_ma,
    bool capability_mismatch,
    bool usb_comms) {
    uint32_t op_units = pd_ma_to_10ma(operating_ma) & PD_RDO_CURRENT_MASK;
    uint32_t max_units = pd_ma_to_10ma(max_ma) & PD_RDO_CURRENT_MASK;

    uint32_t rdo = 0;
    rdo |= ((uint32_t)(object_position & 0x0F)) << PD_RDO_OBJ_POS_SHIFT;
    rdo |= op_units << PD_RDO_OP_CURRENT_SHIFT;
    rdo |= max_units << PD_RDO_MAX_CURRENT_SHIFT;
    if(capability_mismatch) {
        rdo |= 1u << PD_RDO_CAP_MISMATCH_SHIFT;
    }
    if(usb_comms) {
        rdo |= 1u << PD_RDO_USB_COMMS_SHIFT;
    }
    /* Always set No USB Suspend so the source keeps full power available. */
    rdo |= 1u << PD_RDO_NO_USB_SUSPEND_SHIFT;
    return rdo;
}

uint8_t pd_select_fixed_pdo(
    const uint32_t* objects,
    size_t count,
    uint16_t max_voltage_mv,
    PdPdo* selected) {
    uint8_t best_position = 0;
    PdPdo best = {0};

    if(count > PD_MAX_OBJECTS) {
        count = PD_MAX_OBJECTS;
    }

    for(size_t i = 0; i < count; i++) {
        PdPdo pdo;
        pd_pdo_parse(objects[i], &pdo);
        if(pdo.type != PdPdoFixed) {
            continue;
        }
        if(pdo.voltage_mv == 0 || pdo.voltage_mv > max_voltage_mv) {
            continue;
        }
        bool better = false;
        if(best_position == 0) {
            better = true;
        } else if(pdo.voltage_mv > best.voltage_mv) {
            better = true;
        } else if(
            pdo.voltage_mv == best.voltage_mv && pdo.max_current_ma > best.max_current_ma) {
            better = true;
        }
        if(better) {
            best = pdo;
            best_position = (uint8_t)(i + 1);
        }
    }

    if(best_position != 0 && selected != NULL) {
        *selected = best;
    }
    return best_position;
}
