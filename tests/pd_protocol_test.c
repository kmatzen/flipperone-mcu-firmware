/*
 * Host-side unit tests for the hardware-independent USB-PD protocol helpers.
 *
 * These cover the spec-critical bit packing/unpacking that is hard to validate
 * on-target. Build and run on the host:
 *
 *   cc -Wall -Wextra -I../applications/services/pd \
 *      pd_protocol_test.c ../applications/services/pd/pd_protocol.c -o pd_protocol_test
 *   ./pd_protocol_test
 *
 * Expected values are hand-derived from the USB Power Delivery Specification.
 */

#include "pd_protocol.h"

#include <stdio.h>
#include <stdlib.h>

static int g_failures = 0;

#define CHECK_EQ(actual, expected)                                                    \
    do {                                                                              \
        unsigned long _a = (unsigned long)(actual);                                  \
        unsigned long _e = (unsigned long)(expected);                                \
        if(_a != _e) {                                                               \
            printf(                                                                  \
                "FAIL %s:%d: %s = 0x%lX, expected 0x%lX\n", __FILE__, __LINE__, #actual, _a, _e); \
            g_failures++;                                                            \
        }                                                                            \
    } while(0)

#define CHECK_TRUE(cond)                                                  \
    do {                                                                  \
        if(!(cond)) {                                                     \
            printf("FAIL %s:%d: %s is false\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                                 \
        }                                                                 \
    } while(0)

static void test_header_roundtrip(void) {
    PdHeader h = {
        .message_type = PdDataRequest,
        .data_role = PdDataRoleUfp,
        .spec_rev = PdSpecRev30,
        .power_role = PdPowerRoleSink,
        .message_id = 3,
        .num_objects = 1,
        .extended = false,
    };
    uint16_t raw = pd_header_build(&h);
    /* 2 | (2<<6) | (3<<9) | (1<<12) = 0x1682 */
    CHECK_EQ(raw, 0x1682);

    PdHeader parsed;
    pd_header_parse(raw, &parsed);
    CHECK_EQ(parsed.message_type, PdDataRequest);
    CHECK_EQ(parsed.data_role, PdDataRoleUfp);
    CHECK_EQ(parsed.spec_rev, PdSpecRev30);
    CHECK_EQ(parsed.power_role, PdPowerRoleSink);
    CHECK_EQ(parsed.message_id, 3);
    CHECK_EQ(parsed.num_objects, 1);
    CHECK_TRUE(!parsed.extended);

    CHECK_TRUE(!pd_header_is_control(raw));
    /* Accept control message: type 3, no objects. */
    PdHeader accept = {.message_type = PdControlAccept, .spec_rev = PdSpecRev30};
    CHECK_TRUE(pd_header_is_control(pd_header_build(&accept)));
}

static void test_fixed_pdo_decode(void) {
    PdPdo pdo;

    pd_pdo_parse(0x0001912Cu, &pdo); /* 5V / 3A */
    CHECK_EQ(pdo.type, PdPdoFixed);
    CHECK_EQ(pdo.voltage_mv, 5000);
    CHECK_EQ(pdo.max_current_ma, 3000);

    /* Round-trip: building a 5V/3A Fixed PDO yields the canonical word. */
    CHECK_EQ(pd_pdo_build_fixed(5000, 3000), 0x0001912Cu);

    pd_pdo_parse(0x0002D12Cu, &pdo); /* 9V / 3A */
    CHECK_EQ(pdo.voltage_mv, 9000);
    CHECK_EQ(pdo.max_current_ma, 3000);

    pd_pdo_parse(0x000641F4u, &pdo); /* 20V / 5A */
    CHECK_EQ(pdo.voltage_mv, 20000);
    CHECK_EQ(pdo.max_current_ma, 5000);

    /* 5V/3A with Dual-Role Power (bit29) and USB comms (bit26) set. */
    pd_pdo_parse(0x2401912Cu, &pdo);
    CHECK_EQ(pdo.voltage_mv, 5000);
    CHECK_TRUE(pdo.dual_role_power);
    CHECK_TRUE(pdo.usb_comms_capable);
}

static void test_other_pdo_decode(void) {
    PdPdo pdo;

    /* Battery PDO: type 01, max 12V, min 9V, 50W. */
    uint32_t batt = (1u << 30) | (240u << 20) | (180u << 10) | 200u;
    pd_pdo_parse(batt, &pdo);
    CHECK_EQ(pdo.type, PdPdoBattery);
    CHECK_EQ(pdo.voltage_mv, 12000);
    CHECK_EQ(pdo.min_voltage_mv, 9000);
    CHECK_EQ(pdo.max_power_mw, 50000);

    /* Variable PDO: type 10, max 12V, min 5V, 2A. */
    uint32_t var = (2u << 30) | (240u << 20) | (100u << 10) | 200u;
    pd_pdo_parse(var, &pdo);
    CHECK_EQ(pdo.type, PdPdoVariable);
    CHECK_EQ(pdo.voltage_mv, 12000);
    CHECK_EQ(pdo.min_voltage_mv, 5000);
    CHECK_EQ(pdo.max_current_ma, 2000);

    /* PPS APDO: type 11/00, 3.3-11V, 3A. */
    uint32_t pps = (3u << 30) | (110u << 17) | (33u << 8) | 60u;
    pd_pdo_parse(pps, &pdo);
    CHECK_EQ(pdo.type, PdPdoAugmented);
    CHECK_EQ(pdo.voltage_mv, 11000);
    CHECK_EQ(pdo.min_voltage_mv, 3300);
    CHECK_EQ(pdo.max_current_ma, 3000);
}

static void test_rdo_build(void) {
    /* Request PDO #5, 5A op/max, no mismatch, USB comms. */
    uint32_t rdo = pd_rdo_build_fixed(5, 5000, 5000, false, true);
    /* (5<<28) | (500<<10) | 500 | usb_comms(1<<25) | no_suspend(1<<24) */
    CHECK_EQ(rdo, 0x5307D1F4u);

    /* Mismatch flag (bit26) set; PDO #1; 1.5A. */
    uint32_t rdo2 = pd_rdo_build_fixed(1, 1500, 1500, true, false);
    /* (1<<28) | (150<<10) | 150 | mismatch(1<<26) | no_suspend(1<<24) */
    CHECK_EQ(rdo2, 0x15025896u);

    /* Rounding: 2999 mA must round up to 300 units (3000 mA). */
    uint32_t rdo3 = pd_rdo_build_fixed(2, 2999, 2999, false, false);
    CHECK_EQ((rdo3 >> 10) & 0x3FF, 300);
    CHECK_EQ(rdo3 & 0x3FF, 300);
}

static void test_pdo_selection(void) {
    /* 5V/3A, 9V/3A, 15V/3A, 20V/5A */
    const uint32_t caps[] = {0x0001912Cu, 0x0002D12Cu, 0x0004B12Cu, 0x000641F4u};
    const size_t n = sizeof(caps) / sizeof(caps[0]);
    PdPdo chosen;

    CHECK_EQ(pd_select_fixed_pdo(caps, n, 20000, &chosen), 4);
    CHECK_EQ(chosen.voltage_mv, 20000);
    CHECK_EQ(chosen.max_current_ma, 5000);

    CHECK_EQ(pd_select_fixed_pdo(caps, n, 15000, &chosen), 3);
    CHECK_EQ(chosen.voltage_mv, 15000);

    /* No 12V offered: best <= 12V is 9V (position 2). */
    CHECK_EQ(pd_select_fixed_pdo(caps, n, 12000, &chosen), 2);
    CHECK_EQ(chosen.voltage_mv, 9000);

    /* Below the lowest fixed supply: nothing selectable. */
    CHECK_EQ(pd_select_fixed_pdo(caps, n, 4000, &chosen), 0);

    /* Current tie-break at equal voltage: prefer higher current. */
    const uint32_t tie[] = {0x0002D0C8u /* 9V/2A */, 0x0002D12Cu /* 9V/3A */};
    CHECK_EQ(pd_select_fixed_pdo(tie, 2, 9000, &chosen), 2);
    CHECK_EQ(chosen.max_current_ma, 3000);

    /* Non-fixed supplies must be ignored. */
    const uint32_t mixed[] = {
        (1u << 30) | (240u << 20) | (100u << 10) | 200u, /* battery */
        (3u << 30) | (110u << 17) | (33u << 8) | 60u, /* PPS */
    };
    CHECK_EQ(pd_select_fixed_pdo(mixed, 2, 20000, &chosen), 0);
}

static void test_real_charger_pdo_sets(void) {
    PdPdo chosen;

    /* Typical 65 W USB-C GaN charger: 5/9/12/15/20 V Fixed + two PPS APDOs.
     * (20 V is 3.25 A on a 65 W supply.) The PPS objects must be ignored by the
     * Fixed-PDO selector. */
    const uint32_t gan65[] = {
        0x0001912Cu, /* 5V  / 3A    */
        0x0002D12Cu, /* 9V  / 3A    */
        0x0003C12Cu, /* 12V / 3A    */
        0x0004B12Cu, /* 15V / 3A    */
        0x00064145u, /* 20V / 3.25A */
        (3u << 30) | (110u << 17) | (33u << 8) | 60u, /* PPS 3.3-11V / 3A  */
        (3u << 30) | (210u << 17) | (33u << 8) | 100u, /* PPS 3.3-21V / 5A */
    };
    const size_t n = sizeof(gan65) / sizeof(gan65[0]);

    CHECK_EQ(pd_select_fixed_pdo(gan65, n, 9000, &chosen), 2);
    CHECK_EQ(chosen.voltage_mv, 9000);
    CHECK_EQ(pd_select_fixed_pdo(gan65, n, 12000, &chosen), 3);
    CHECK_EQ(chosen.voltage_mv, 12000);
    CHECK_EQ(pd_select_fixed_pdo(gan65, n, 20000, &chosen), 5);
    CHECK_EQ(chosen.voltage_mv, 20000);
    CHECK_EQ(chosen.max_current_ma, 3250);
    /* Far above any offer: still the highest Fixed supply (20V). */
    CHECK_EQ(pd_select_fixed_pdo(gan65, n, 60000, &chosen), 5);

    /* Apple 20 W: 5V/3A + 9V/2.22A. */
    const uint32_t apple20[] = {0x0001912Cu, 0x0002D0DEu};
    CHECK_EQ(pd_select_fixed_pdo(apple20, 2, 9000, &chosen), 2);
    CHECK_EQ(chosen.voltage_mv, 9000);
    CHECK_EQ(chosen.max_current_ma, 2220);

    /* 5 V-only brick: nothing above 5 V, so 5 V wins; below 5 V, nothing. */
    const uint32_t brick[] = {0x0001912Cu};
    CHECK_EQ(pd_select_fixed_pdo(brick, 1, 20000, &chosen), 1);
    CHECK_EQ(pd_select_fixed_pdo(brick, 1, 4500, &chosen), 0);
}

int main(void) {
    test_header_roundtrip();
    test_fixed_pdo_decode();
    test_other_pdo_decode();
    test_rdo_build();
    test_pdo_selection();
    test_real_charger_pdo_sets();

    if(g_failures == 0) {
        printf("All PD protocol tests passed.\n");
        return EXIT_SUCCESS;
    }
    printf("%d PD protocol test(s) failed.\n", g_failures);
    return EXIT_FAILURE;
}
