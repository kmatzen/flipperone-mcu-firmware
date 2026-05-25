/*
 * Host-side simulation of the USB-PD sink policy engine.
 *
 * Drives the hardware-free policy (applications/services/pd/pd_sink_policy.c)
 * through full negotiation flows with simulated source messages, with no target
 * hardware. Build and run on the host:
 *
 *   cc -Wall -Wextra -I../applications/services/pd pd_sink_policy_test.c \
 *      ../applications/services/pd/pd_sink_policy.c \
 *      ../applications/services/pd/pd_protocol.c -o pd_sink_policy_test
 *   ./pd_sink_policy_test
 */

#include "pd_sink_policy.h"
#include "pd_protocol.h"

#include <stdio.h>
#include <stdlib.h>

static int g_failures = 0;

#define CHECK_EQ(actual, expected)                                                                \
    do {                                                                                          \
        unsigned long _a = (unsigned long)(actual);                                              \
        unsigned long _e = (unsigned long)(expected);                                            \
        if(_a != _e) {                                                                           \
            printf("FAIL %s:%d: %s = 0x%lX, expected 0x%lX\n", __FILE__, __LINE__, #actual, _a, _e); \
            g_failures++;                                                                        \
        }                                                                                        \
    } while(0)

#define CHECK_TRUE(cond)                                                   \
    do {                                                                   \
        if(!(cond)) {                                                      \
            printf("FAIL %s:%d: %s is false\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                                  \
        }                                                                  \
    } while(0)

/* Build a header as a Source (DFP) would send. */
static uint16_t src_header(uint8_t msg_type, uint8_t msg_id, uint8_t num_obj) {
    PdHeader h = {
        .message_type = msg_type,
        .data_role = PdDataRoleDfp,
        .spec_rev = PdSpecRev20,
        .power_role = PdPowerRoleSource,
        .message_id = msg_id,
        .num_objects = num_obj,
        .extended = false,
    };
    return pd_header_build(&h);
}

/* Common Source PDOs (Fixed). */
#define PDO_5V_3A 0x0001912Cu
#define PDO_9V_3A 0x0002D12Cu
#define PDO_20V_5A 0x000641F4u

static void test_full_negotiation(void) {
    PdSinkPolicy policy;
    PdSinkAction action;
    pd_sink_policy_init(&policy, 9000); /* ceiling 9V */

    pd_sink_policy_attach(&policy, &action);
    CHECK_EQ(pd_sink_policy_state(&policy), PdSinkStateWaitCaps);
    CHECK_EQ(action.outcome, PdSinkOutcomeAttached);
    CHECK_TRUE(!action.send);

    /* Source_Capabilities -> we should Request the 9V Fixed PDO (position 2). */
    const uint32_t caps[] = {PDO_5V_3A, PDO_9V_3A, PDO_20V_5A};
    pd_sink_policy_handle_message(
        &policy, src_header(PdDataSourceCapabilities, 0, 3), caps, 3, &action);
    CHECK_TRUE(action.send);
    CHECK_EQ(action.tx_message_type, PdDataRequest);
    CHECK_EQ(action.tx_object_count, 1);
    CHECK_EQ(action.tx_objects[0], 0x2304B12Cu); /* PDO#2, 3A op/max, usb-comms, no-suspend */
    CHECK_EQ(action.tx_message_id, 0);
    CHECK_EQ(pd_sink_policy_state(&policy), PdSinkStateRequested);

    /* Accept -> wait for PS_RDY, nothing to send. */
    pd_sink_policy_handle_message(&policy, src_header(PdControlAccept, 1, 0), NULL, 0, &action);
    CHECK_TRUE(!action.send);
    CHECK_EQ(pd_sink_policy_state(&policy), PdSinkStateWaitPsRdy);

    /* PS_RDY -> contract in effect. */
    pd_sink_policy_handle_message(&policy, src_header(PdControlPsRdy, 2, 0), NULL, 0, &action);
    CHECK_EQ(pd_sink_policy_state(&policy), PdSinkStateReady);
    CHECK_EQ(action.outcome, PdSinkOutcomeContract);
    CHECK_EQ(action.contract_voltage_mv, 9000);
    CHECK_EQ(action.contract_current_ma, 3000);

    uint16_t v = 0, c = 0;
    CHECK_TRUE(pd_sink_policy_contract(&policy, &v, &c));
    CHECK_EQ(v, 9000);
    CHECK_EQ(c, 3000);

    /* Get_Sink_Cap -> reply Sink_Capabilities (5V), MessageID continues (1). */
    pd_sink_policy_handle_message(&policy, src_header(PdControlGetSinkCap, 3, 0), NULL, 0, &action);
    CHECK_TRUE(action.send);
    CHECK_EQ(action.tx_message_type, PdDataSinkCapabilities);
    CHECK_EQ(action.tx_objects[0], 0x0001912Cu); /* 5V / 3A */
    CHECK_EQ(action.tx_message_id, 1);
}

static void test_soft_reset(void) {
    PdSinkPolicy policy;
    PdSinkAction action;
    pd_sink_policy_init(&policy, 9000);
    pd_sink_policy_attach(&policy, &action);

    const uint32_t caps[] = {PDO_5V_3A, PDO_9V_3A};
    pd_sink_policy_handle_message(
        &policy, src_header(PdDataSourceCapabilities, 0, 2), caps, 2, &action);
    CHECK_EQ(action.tx_message_id, 0); /* consumed id 0 */

    /* Soft_Reset -> reset MessageID to 0 and Accept. */
    pd_sink_policy_handle_message(&policy, src_header(PdControlSoftReset, 5, 0), NULL, 0, &action);
    CHECK_TRUE(action.send);
    CHECK_EQ(action.tx_message_type, PdControlAccept);
    CHECK_EQ(action.tx_message_id, 0); /* counter was reset before sending Accept */
    CHECK_EQ(pd_sink_policy_state(&policy), PdSinkStateWaitCaps);
}

static void test_capability_mismatch_floor(void) {
    /* Ceiling below vSafe5V: nothing selectable, so request PDO#1 with mismatch. */
    PdSinkPolicy policy;
    PdSinkAction action;
    pd_sink_policy_init(&policy, 4000);
    pd_sink_policy_attach(&policy, &action);

    const uint32_t caps[] = {PDO_5V_3A, PDO_9V_3A};
    pd_sink_policy_handle_message(
        &policy, src_header(PdDataSourceCapabilities, 0, 2), caps, 2, &action);
    CHECK_TRUE(action.send);
    CHECK_EQ(action.tx_message_type, PdDataRequest);
    CHECK_EQ(action.tx_objects[0], 0x1704B12Cu); /* PDO#1, 3A, mismatch flag set */
    CHECK_EQ(pd_sink_policy_state(&policy), PdSinkStateRequested);

    pd_sink_policy_handle_message(&policy, src_header(PdControlAccept, 1, 0), NULL, 0, &action);
    pd_sink_policy_handle_message(&policy, src_header(PdControlPsRdy, 2, 0), NULL, 0, &action);
    CHECK_EQ(action.contract_voltage_mv, 5000); /* fell back to 5V */
}

static void test_reject_and_wait(void) {
    PdSinkPolicy policy;
    PdSinkAction action;
    pd_sink_policy_init(&policy, 9000);
    pd_sink_policy_attach(&policy, &action);

    const uint32_t caps[] = {PDO_5V_3A, PDO_9V_3A};
    pd_sink_policy_handle_message(
        &policy, src_header(PdDataSourceCapabilities, 0, 2), caps, 2, &action);
    CHECK_EQ(pd_sink_policy_state(&policy), PdSinkStateRequested);

    /* Reject -> back to waiting for fresh capabilities, no contract. */
    pd_sink_policy_handle_message(&policy, src_header(PdControlReject, 1, 0), NULL, 0, &action);
    CHECK_EQ(pd_sink_policy_state(&policy), PdSinkStateWaitCaps);
    CHECK_TRUE(!pd_sink_policy_contract(&policy, NULL, NULL));
}

static void test_detach_and_hard_reset(void) {
    PdSinkPolicy policy;
    PdSinkAction action;
    pd_sink_policy_init(&policy, 20000);
    pd_sink_policy_attach(&policy, &action);

    const uint32_t caps[] = {PDO_5V_3A, PDO_9V_3A, PDO_20V_5A};
    pd_sink_policy_handle_message(
        &policy, src_header(PdDataSourceCapabilities, 0, 3), caps, 3, &action);
    CHECK_EQ(action.tx_objects[0] >> 28, 3); /* selected PDO #3 (20V) */
    pd_sink_policy_handle_message(&policy, src_header(PdControlAccept, 1, 0), NULL, 0, &action);
    pd_sink_policy_handle_message(&policy, src_header(PdControlPsRdy, 2, 0), NULL, 0, &action);
    CHECK_EQ(pd_sink_policy_state(&policy), PdSinkStateReady);

    /* Hard reset while a contract is active -> Failed, back to WaitCaps. */
    pd_sink_policy_hard_reset(&policy, &action);
    CHECK_EQ(action.outcome, PdSinkOutcomeFailed);
    CHECK_EQ(pd_sink_policy_state(&policy), PdSinkStateWaitCaps);

    /* Re-negotiate, then detach -> Detached, back to Idle. */
    pd_sink_policy_handle_message(
        &policy, src_header(PdDataSourceCapabilities, 0, 3), caps, 3, &action);
    pd_sink_policy_handle_message(&policy, src_header(PdControlAccept, 1, 0), NULL, 0, &action);
    pd_sink_policy_handle_message(&policy, src_header(PdControlPsRdy, 2, 0), NULL, 0, &action);
    CHECK_EQ(pd_sink_policy_state(&policy), PdSinkStateReady);

    pd_sink_policy_detach(&policy, &action);
    CHECK_EQ(action.outcome, PdSinkOutcomeDetached);
    CHECK_EQ(pd_sink_policy_state(&policy), PdSinkStateIdle);
    CHECK_TRUE(!pd_sink_policy_contract(&policy, NULL, NULL));
}

static void test_out_of_order_ignored(void) {
    /* Messages that don't fit the current state must be ignored. */
    PdSinkPolicy policy;
    PdSinkAction action;
    pd_sink_policy_init(&policy, 9000);
    pd_sink_policy_attach(&policy, &action); /* WaitCaps */

    pd_sink_policy_handle_message(&policy, src_header(PdControlPsRdy, 0, 0), NULL, 0, &action);
    CHECK_EQ(pd_sink_policy_state(&policy), PdSinkStateWaitCaps);
    CHECK_TRUE(!action.send);
    CHECK_EQ(action.outcome, PdSinkOutcomeNone);

    pd_sink_policy_handle_message(&policy, src_header(PdControlAccept, 0, 0), NULL, 0, &action);
    CHECK_EQ(pd_sink_policy_state(&policy), PdSinkStateWaitCaps);
}

int main(void) {
    test_full_negotiation();
    test_soft_reset();
    test_capability_mismatch_floor();
    test_reject_and_wait();
    test_detach_and_hard_reset();
    test_out_of_order_ignored();

    if(g_failures == 0) {
        printf("All PD sink policy tests passed.\n");
        return EXIT_SUCCESS;
    }
    printf("%d PD sink policy test(s) failed.\n", g_failures);
    return EXIT_FAILURE;
}
