#include "pd_cli.h"

#include <args.h>
#include <pd/pd.h>
#include <cli/cli_ansi.h>

static const char* pd_cli_mode_str(PdMode mode) {
    switch(mode) {
    case PdModeOff:
        return "Off";
    case PdModeDrp:
        return "DRP";
    case PdModeSnk:
        return "Sink";
    case PdModeSrc:
        return "Source";
    default:
        return "Unknown";
    }
}

static void pd_cli_print_usage(void) {
    printf("Usage:\r\n");
    printf("  pd status        live PD status (Ctrl-C to exit)\r\n");
    printf("  pd snk [max_mv]  start sink negotiation (optional voltage ceiling)\r\n");
    printf("  pd off           disable PD\r\n");
}

static void pd_cli_status_loop(Cli* cli, Pd* pd) {
    printf(ANSI_ERASE_DISPLAY(ANSI_ERASE_ENTIRE));
    while(!cli_cmd_interrupt_received(cli)) {
        printf(ANSI_CURSOR_POS("0", "0"));

        PdMode mode = PdModeOff;
        pd_get_mode(pd, &mode);

        uint16_t voltage_mv = 0;
        uint16_t current_ma = 0;
        bool contract = pd_get_contract(pd, &voltage_mv, &current_ma);

        printf(ANSI_ERASE_LINE(ANSI_ERASE_ENTIRE) "PD:\r\n");
        printf(ANSI_ERASE_LINE(ANSI_ERASE_ENTIRE) "  Mode:     %s\r\n", pd_cli_mode_str(mode));
        if(contract) {
            printf(
                ANSI_ERASE_LINE(ANSI_ERASE_ENTIRE) "  Contract: %u mV  %u mA\r\n",
                voltage_mv,
                current_ma);
        } else {
            printf(ANSI_ERASE_LINE(ANSI_ERASE_ENTIRE) "  Contract: none\r\n");
        }
        furi_delay_ms(500);
    }
}

void pd_cli(Cli* cli, FuriString* args, void* context) {
    UNUSED(context);
    Pd* pd = furi_record_open(RECORD_PD);

    FuriString* cmd = furi_string_alloc();
    do {
        if(!args_read_string_and_trim(args, cmd) || furi_string_equal(cmd, "status")) {
            pd_cli_status_loop(cli, pd);
            break;
        }

        if(furi_string_equal(cmd, "off")) {
            bool ok = pd_set_mode(pd, PdModeOff);
            printf("PD off: %s\r\n", ok ? "ok" : "failed");
            break;
        }

        if(furi_string_equal(cmd, "snk")) {
            int max_mv = 0;
            if(args_read_int_and_trim(args, &max_mv) && max_mv > 0) {
                if(!pd_set_sink_max_voltage(pd, (uint16_t)max_mv)) {
                    printf("Failed to set max voltage\r\n");
                    break;
                }
                printf("Sink voltage ceiling: %d mV\r\n", max_mv);
            }
            bool ok = pd_set_mode(pd, PdModeSnk);
            printf("PD sink: %s\r\n", ok ? "ok" : "failed");
            break;
        }

        pd_cli_print_usage();
    } while(false);

    furi_string_free(cmd);
    furi_record_close(RECORD_PD);
}
