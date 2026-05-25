#include "pd_sink_policy.h"

/** vSafe5V is the guaranteed first Source PDO; used as the safe fallback. */
#define PD_SINK_FALLBACK_CURRENT_MA (500)

static void pd_sink_action_clear(PdSinkAction* action) {
    action->send = false;
    action->tx_message_type = 0;
    action->tx_message_id = 0;
    action->tx_object_count = 0;
    for(uint8_t i = 0; i < PD_MAX_OBJECTS; i++) {
        action->tx_objects[i] = 0;
    }
    action->outcome = PdSinkOutcomeNone;
    action->contract_voltage_mv = 0;
    action->contract_current_ma = 0;
}

/** Stage a message for transmission, consuming the next MessageID. */
static void pd_sink_action_send(
    PdSinkPolicy* policy,
    PdSinkAction* action,
    uint8_t message_type,
    const uint32_t* objects,
    uint8_t count) {
    action->send = true;
    action->tx_message_type = message_type;
    action->tx_message_id = policy->tx_msg_id;
    action->tx_object_count = count;
    for(uint8_t i = 0; i < count; i++) {
        action->tx_objects[i] = objects[i];
    }
    policy->tx_msg_id = (policy->tx_msg_id + 1) & 0x07;
}

void pd_sink_policy_init(PdSinkPolicy* policy, uint16_t max_voltage_mv) {
    policy->max_voltage_mv = max_voltage_mv;
    pd_sink_policy_reset(policy);
}

void pd_sink_policy_reset(PdSinkPolicy* policy) {
    policy->state = PdSinkStateIdle;
    policy->tx_msg_id = 0;
    policy->selected_position = 0;
    policy->selected_voltage_mv = 0;
    policy->selected_current_ma = 0;
    policy->contract_voltage_mv = 0;
    policy->contract_current_ma = 0;
}

void pd_sink_policy_set_max_voltage(PdSinkPolicy* policy, uint16_t max_voltage_mv) {
    policy->max_voltage_mv = max_voltage_mv;
}

void pd_sink_policy_attach(PdSinkPolicy* policy, PdSinkAction* action) {
    pd_sink_action_clear(action);
    policy->state = PdSinkStateWaitCaps;
    policy->tx_msg_id = 0;
    policy->contract_voltage_mv = 0;
    policy->contract_current_ma = 0;
    action->outcome = PdSinkOutcomeAttached;
}

void pd_sink_policy_detach(PdSinkPolicy* policy, PdSinkAction* action) {
    pd_sink_action_clear(action);
    bool had_contract = (policy->state == PdSinkStateReady);
    pd_sink_policy_reset(policy);
    if(had_contract) {
        action->outcome = PdSinkOutcomeDetached;
    }
}

void pd_sink_policy_hard_reset(PdSinkPolicy* policy, PdSinkAction* action) {
    pd_sink_action_clear(action);
    bool had_contract = (policy->state == PdSinkStateReady);
    policy->tx_msg_id = 0;
    policy->contract_voltage_mv = 0;
    policy->contract_current_ma = 0;
    policy->state = PdSinkStateWaitCaps;
    if(had_contract) {
        action->outcome = PdSinkOutcomeFailed;
    }
}

/** Select and request the best Fixed PDO (or fall back to vSafe5V on mismatch). */
static void pd_sink_policy_request(
    PdSinkPolicy* policy,
    const uint32_t* objects,
    uint8_t count,
    PdSinkAction* action) {
    PdPdo chosen;
    uint8_t position = pd_select_fixed_pdo(objects, count, policy->max_voltage_mv, &chosen);

    uint32_t rdo;
    if(position == 0) {
        /* Nothing within the ceiling: request PDO #1 (always vSafe5V per spec)
         * and flag a capability mismatch. */
        PdPdo first;
        pd_pdo_parse(objects[0], &first);
        uint16_t current =
            first.max_current_ma ? first.max_current_ma : PD_SINK_FALLBACK_CURRENT_MA;
        rdo = pd_rdo_build_fixed(1, current, current, true, true);
        policy->selected_position = 1;
        policy->selected_voltage_mv = first.voltage_mv;
        policy->selected_current_ma = current;
    } else {
        uint16_t current = chosen.max_current_ma;
        if(current > PD_SINK_POLICY_MAX_CURRENT_MA) {
            current = PD_SINK_POLICY_MAX_CURRENT_MA;
        }
        rdo = pd_rdo_build_fixed(position, current, current, false, true);
        policy->selected_position = position;
        policy->selected_voltage_mv = chosen.voltage_mv;
        policy->selected_current_ma = current;
    }

    pd_sink_action_send(policy, action, PdDataRequest, &rdo, 1);
    policy->state = PdSinkStateRequested;
}

void pd_sink_policy_handle_message(
    PdSinkPolicy* policy,
    uint16_t header,
    const uint32_t* objects,
    uint8_t count,
    PdSinkAction* action) {
    pd_sink_action_clear(action);

    PdHeader parsed;
    pd_header_parse(header, &parsed);

    if(pd_header_is_control(header)) {
        switch(parsed.message_type) {
        case PdControlAccept:
            if(policy->state == PdSinkStateRequested) {
                policy->state = PdSinkStateWaitPsRdy;
            }
            break;
        case PdControlPsRdy:
            if(policy->state == PdSinkStateWaitPsRdy) {
                policy->state = PdSinkStateReady;
                policy->contract_voltage_mv = policy->selected_voltage_mv;
                policy->contract_current_ma = policy->selected_current_ma;
                action->outcome = PdSinkOutcomeContract;
                action->contract_voltage_mv = policy->contract_voltage_mv;
                action->contract_current_ma = policy->contract_current_ma;
            }
            break;
        case PdControlReject:
        case PdControlWait:
            /* Keep the previous state of affairs; await fresh capabilities. */
            if(policy->state == PdSinkStateRequested) {
                policy->state = PdSinkStateWaitCaps;
            }
            break;
        case PdControlSoftReset:
            /* Reset our MessageID counter and acknowledge (spec section 6.8.2). */
            policy->tx_msg_id = 0;
            pd_sink_action_send(policy, action, PdControlAccept, NULL, 0);
            policy->state = PdSinkStateWaitCaps;
            break;
        case PdControlGetSinkCap: {
            uint32_t pdo = pd_pdo_build_fixed(5000, PD_SINK_POLICY_MAX_CURRENT_MA);
            pd_sink_action_send(policy, action, PdDataSinkCapabilities, &pdo, 1);
            break;
        }
        default:
            break;
        }
    } else {
        if(parsed.message_type == PdDataSourceCapabilities && count > 0) {
            pd_sink_policy_request(policy, objects, count, action);
        }
    }
}

PdSinkState pd_sink_policy_state(const PdSinkPolicy* policy) {
    return policy->state;
}

bool pd_sink_policy_contract(
    const PdSinkPolicy* policy,
    uint16_t* voltage_mv,
    uint16_t* current_ma) {
    bool active = (policy->state == PdSinkStateReady) && (policy->contract_voltage_mv > 0);
    if(active) {
        if(voltage_mv) {
            *voltage_mv = policy->contract_voltage_mv;
        }
        if(current_ma) {
            *current_ma = policy->contract_current_ma;
        }
    }
    return active;
}
