#pragma once

/**
 * USB Power Delivery sink policy engine — hardware independent.
 *
 * This module owns the sink negotiation state machine and all the decisions:
 * which PDO to request, when to transition, what to transmit and which outcome
 * to report. It deliberately has no FUSB302 / FreeRTOS dependency so the whole
 * negotiation can be simulated and unit-tested on the host
 * (see tests/pd_sink_policy_test.c).
 *
 * The caller (the `pd` service) drives it by feeding events (attach, detach,
 * hard reset, received messages) and executing the returned PdSinkAction:
 * transmitting the described PD message and/or publishing the described outcome.
 */

#include "pd_protocol.h"

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Current operating current limit the sink advertises/requests, mA. */
#define PD_SINK_POLICY_MAX_CURRENT_MA (3000)

/** Sink negotiation state. */
typedef enum {
    PdSinkStateIdle, //!< no source / not negotiating
    PdSinkStateWaitCaps, //!< source present, waiting for Source_Capabilities
    PdSinkStateRequested, //!< Request sent, waiting for Accept
    PdSinkStateWaitPsRdy, //!< Accept received, waiting for PS_RDY
    PdSinkStateReady, //!< power contract in effect
} PdSinkState;

/** High-level result of an event, mapped by the caller to a pubsub event. */
typedef enum {
    PdSinkOutcomeNone,
    PdSinkOutcomeAttached, //!< a source was detected; negotiation started
    PdSinkOutcomeContract, //!< a contract is in effect (voltage/current valid)
    PdSinkOutcomeDetached, //!< source detached, contract lost
    PdSinkOutcomeFailed, //!< negotiation failed (hard reset with active contract)
} PdSinkOutcome;

/** What the caller should do after an event. */
typedef struct {
    bool send; //!< transmit a PD message
    uint8_t tx_message_type; //!< PdControl* / PdData* message type
    uint8_t tx_message_id; //!< MessageID to place in the header
    uint8_t tx_object_count; //!< number of data objects (0 for control)
    uint32_t tx_objects[PD_MAX_OBJECTS];

    PdSinkOutcome outcome; //!< event to publish (PdSinkOutcomeNone = nothing)
    uint16_t contract_voltage_mv; //!< valid when outcome == PdSinkOutcomeContract
    uint16_t contract_current_ma;
} PdSinkAction;

/** Policy state. Treat as opaque; modified only through the functions below. */
typedef struct {
    PdSinkState state;
    uint8_t tx_msg_id;
    uint16_t max_voltage_mv;
    uint8_t selected_position;
    uint16_t selected_voltage_mv;
    uint16_t selected_current_ma;
    uint16_t contract_voltage_mv;
    uint16_t contract_current_ma;
} PdSinkPolicy;

/** Initialise the policy with a voltage ceiling (mV) and clear all state. */
void pd_sink_policy_init(PdSinkPolicy* policy, uint16_t max_voltage_mv);

/** Reset negotiation state (state, contract, MessageID); keep the voltage ceiling. */
void pd_sink_policy_reset(PdSinkPolicy* policy);

/** Update the requested voltage ceiling (mV); applies to the next negotiation. */
void pd_sink_policy_set_max_voltage(PdSinkPolicy* policy, uint16_t max_voltage_mv);

/** A source was detected (VBUS up, orientation known). -> WaitCaps, Attached. */
void pd_sink_policy_attach(PdSinkPolicy* policy, PdSinkAction* action);

/** Source detached. -> Idle, Detached if a contract was active. */
void pd_sink_policy_detach(PdSinkPolicy* policy, PdSinkAction* action);

/** PD hard reset received. -> WaitCaps, Failed if a contract was active. */
void pd_sink_policy_hard_reset(PdSinkPolicy* policy, PdSinkAction* action);

/**
 * Handle a received SOP PD message.
 *
 * @param header  raw 16-bit message header
 * @param objects data objects (may be NULL when count == 0)
 * @param count   number of data objects
 * @param action  [out] resulting action (always written)
 */
void pd_sink_policy_handle_message(
    PdSinkPolicy* policy,
    uint16_t header,
    const uint32_t* objects,
    uint8_t count,
    PdSinkAction* action);

/** Current negotiation state. */
PdSinkState pd_sink_policy_state(const PdSinkPolicy* policy);

/**
 * Read the active contract.
 * @return true if a contract is in effect; voltage_mv/current_ma are then filled.
 */
bool pd_sink_policy_contract(
    const PdSinkPolicy* policy,
    uint16_t* voltage_mv,
    uint16_t* current_ma);

#ifdef __cplusplus
}
#endif
