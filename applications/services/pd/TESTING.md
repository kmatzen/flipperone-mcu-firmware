# Testing the PD (FUSB302 sink) service

Three levels: host logic tests (no hardware), on-target tests (real Flipper One
MCU board), and a DIY bring-up rig if no board is available yet.

---

## 1. Host logic tests (no hardware)

The spec-critical logic is hardware-independent and unit-tested on the dev
machine. See [`tests/README.md`](../../../tests/README.md).

```sh
cd tests
cc -Wall -Wextra -I../applications/services/pd \
   pd_protocol_test.c ../applications/services/pd/pd_protocol.c -o pd_protocol_test && ./pd_protocol_test
cc -Wall -Wextra -I../applications/services/pd pd_sink_policy_test.c \
   ../applications/services/pd/pd_sink_policy.c ../applications/services/pd/pd_protocol.c \
   -o pd_sink_policy_test && ./pd_sink_policy_test
```

These cover PDO/RDO/header bit-packing and a full simulated negotiation
(`Source_Capabilities → Request → Accept → PS_RDY`, soft reset, mismatch,
detach, hard reset). They prove the *logic*; they cannot prove chip timing or
analog behaviour.

---

## 2. On-target test (Flipper One MCU board)

### Flash
- **BOOTSEL:** hold BOOTSEL, plug USB → drag `build/flipperone-mcu-firmware.uf2`
  onto the RPI-RP2 drive.
- or `picotool load build/flipperone-mcu-firmware.uf2 -fx`
- or the VS Code "Flash" task (openocd / CMSIS-DAP SWD probe).

### Connect
- **CLI** is a USB CDC virtual serial port: `ls /dev/cu.usbmodem*` then
  `tio /dev/cu.usbmodemXXXX` (or `screen`).
- **Logs** (`FURI_LOG`: `Source attached`, `Selected PDO`, `PD contract:`, errors)
  go out the **debug UART** — hook a UART adapter to see them (optional but
  recommended).

### Procedure
Negotiation is **not** auto-started at boot — run `pd snk` each session.

| Step | Command / action | Expected |
|---|---|---|
| Safe handshake first | `pd snk 5000`, then `pd status` | Forces a 5V-only request — exercises Request→Accept→PS_RDY **without changing VBUS**. Contract shows `5000 mV …` |
| Real negotiation | `pd off`; `pd snk 9000`; `pd status`; plug a multi-PDO PD charger | Contract populates (e.g. `9000 mV 3000 mA`); logs show `Source attached on CC1`, `Selected PDO #2`, `PD contract:` |
| Cross-check it's real | run `power` while a contract is up | VBUS rises from ~5V to the negotiated voltage — proves the source actually switched |
| Orientation | flip the USB-C cable, repeat | still negotiates (detects CC2) |
| Detach | unplug | `pd status` → `none`; log `Source detached` |
| Disable | `pd off` | returns to off |

**Success criteria:** `pd status` shows a contract AND `power` confirms VBUS
moved to that voltage.

### Safety
Keep the ceiling at/below the board's VBUS rating. Start at `pd snk 5000`, then
`9000`. Do **not** request 20V until the VBUS path and the BQ25792 `vac_ovp`
setting are confirmed to tolerate it. Default ceiling is
`PD_SINK_DEFAULT_MAX_VOLTAGE_MV` (9000 mV) in `pd.c`.

### If it doesn't negotiate
- **No interrupt** → on this board the FUSB302 INT routes through the I/O
  expander (`furi_bsp_expander_main_attach_fusb302_callback`); confirm that path.
- **Orientation wrong** → check `fusb302_detect_cc_orientation` BC_LVL thresholds.
- **Source ignores us** → verify the Switches1 GoodCRC role bits (sink/UFP/Rev2.0)
  in `fusb302_pd_sink_start`.
- A USB-PD analyzer (e.g. ChargerLAB POWER-Z KM003C) on CC shows both sides of
  the exchange and is the fastest way to localise a failure.

---

## 3. DIY bring-up rig (RP2350, no Flipper One board)

This firmware targets the **RP2350** (Cortex-M33, Armv8-M; the build is
`cortex-m33 -mcmse`, FreeRTOS `RP2350_ARM_NTZ`, and the real board uses a
PGA2350 / RP2350B module). A DIY test rig should therefore also be **RP2350-based**
so the toolchain, `pico-sdk` and furi HAL all match — no architecture port needed.
(An RP2040 is a different architecture — Cortex-M0+, Armv6-M — and is not the
target; the f100 image will not run on it.)

**Recommended rig:** a **Raspberry Pi Pico 2 (RP2350)** + a FUSB302 breakout.

Because the MCU family matches the target:
- The same build flow applies (`PICO_SDK_PATH`, `arm-none-eabi-gcc`,
  `cmake -G Ninja`, build type `Release`).
- The FUSB302 driver and both pure PD modules (`pd_protocol.c`,
  `pd_sink_policy.c`) compile and run as-is — you adapt **board config + wiring**,
  not code architecture.
- You will need to disable/stub the peripherals the f100 board has but a bare
  Pico 2 does not (display, I/O expander, charger, fuel gauge) to boot a minimal
  image, or build a small standalone harness that just starts the `pd` service on
  the FUSB302.

**Wiring (any free I²C + one GPIO):**
- FUSB302 `SDA`/`SCL` → an I²C bus (7-bit address **0x22**, see `FUSB302_ADDRESS`).
- FUSB302 `INT_N` → a GPIO. On a DIY board wire it **directly** and pass that pin
  as `fusb302_init(..., pin_interrupt)` (the service passes `NULL` because the
  real board routes INT through the PCAL6416 I/O expander). The driver already
  supports a direct INT pin.
- FUSB302 `CC1`/`CC2` → the USB-C receptacle CC pins; `VBUS` to the receptacle
  VBUS (the FUSB302 senses VBUS internally — no extra divider needed).

**Limitations of a DIY rig:**
- No BQ25792, so you can't reproduce the full charge loop. Read the negotiated
  state via the FUSB302's own VBUS sense / the reported contract, and use an
  inline USB-PD analyzer to confirm the real voltage on VBUS.
- You still need a multi-PDO PD charger and a full-featured (CC-passthrough) C-to-C
  cable.
