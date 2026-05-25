# USB-PD sink dev rig (Raspberry Pi Pico 2 + FUSB302)

A standalone bring-up rig to validate the project's USB-PD sink **before the
Flipper One MCU board exists**. It compiles the *real* firmware code —
`lib/drivers/fusb302/fusb302.c`, `pd_sink_policy.c`, `pd_protocol.c` — against a
bare `pico-sdk` project via a thin furi→pico-sdk shim (`compat/`), and drives the
FUSB302 by polling. It prints the negotiated contract over USB serial.

What it covers: the FUSB302 driver (orientation detect, sink config, message
TX/RX) and the full negotiation (`Source_Capabilities → Request → Accept →
PS_RDY`, soft/hard reset). What it does **not** cover: the BQ25792 charger
integration — that's board-level (see `../applications/services/pd/TESTING.md`).

## Bill of materials
- Raspberry Pi Pico 2 (RP2350).
- A FUSB302 breakout that brings out CC1/CC2 to a USB-C receptacle, plus VBUS,
  INT, SDA, SCL, VDD, GND.
- A multi-PDO USB-C PD charger (offers 9V/12V/… , ideally PPS).
- A full-featured (CC-passthrough) USB-C ↔ C cable.
- Recommended: an inline USB-PD analyzer / USB-C V/A meter to confirm the actual
  VBUS voltage (the rig has no calibrated voltage ADC).

## Wiring
| Pico 2 | FUSB302 breakout |
|---|---|
| GP4 (pin 6) | SDA |
| GP5 (pin 7) | SCL |
| 3V3 (OUT) | VDD |
| GND | GND |

- Add 4.7 kΩ pull-ups from SDA/SCL to 3V3 if the breakout doesn't have them.
- FUSB302 `CC1`/`CC2` → the USB-C receptacle CC pins; `VBUS` → receptacle VBUS
  (the FUSB302 senses VBUS internally — no divider needed).
- `INT` is **not** required: the rig polls. (To switch to interrupt-driven, wire
  INT to a Pico GPIO and flesh out `compat/furi_hal_gpio.c` + pass the pin to
  `fusb302_init`.)
- Power the Pico 2 from your computer's USB (that port is also the serial
  console). The **charger** plugs into the FUSB302 breakout's USB-C receptacle —
  these are two separate USB connections.

Pins are configurable at the top of `main.c` (`I2C_*`) and the requested voltage
ceiling via `SINK_MAX_VOLTAGE_MV`.

## Build
From the repo root, with an arm-none-eabi toolchain and a pico-sdk available:

```sh
export PICO_SDK_PATH=$PWD/pico-sdk
export PATH=$PWD/toolchain/arm-none-eabi-gcc/bin:$PATH
cmake -S dev_rig -B dev_rig/build -G Ninja
cmake --build dev_rig/build
```

## Flash & run
1. Hold **BOOTSEL**, plug the Pico 2 in, release → it mounts as `RP2350`.
2. Drag `dev_rig/build/pd_dev_rig.uf2` onto it (or `picotool load ... -fx`).
3. Open the Pico's USB serial: `tio /dev/cu.usbmodem*` (or `screen`).
4. Plug a PD charger into the FUSB302 receptacle.

Expected serial output:
```
[rig] FUSB302 USB-PD sink bring-up rig
[rig] armed, ceiling 9000 mV — plug in a PD charger
[rig] attach on CC1
[rig] CONTRACT: 9000 mV  3000 mA
```
Confirm with the inline analyzer/meter that VBUS actually rises to the negotiated
voltage. Unplugging prints `[rig] detached`.

## Safety
Keep `SINK_MAX_VOLTAGE_MV` within what your wiring/breakout can handle. Start at
5000, then 9000, before trying higher.
