# Host tests

Host-compilable unit tests for hardware-independent firmware logic. These run on
the development machine (no target hardware required).

## USB-PD protocol (`pd_protocol_test.c`)

Covers the spec-critical bit packing/unpacking in
`applications/services/pd/pd_protocol.c`: message-header build/parse, Source PDO
decoding (Fixed / Battery / Variable / PPS), Request Data Object building, and
Fixed-PDO selection. Expected values are hand-derived from the USB Power Delivery
Specification.

```sh
cc -Wall -Wextra -I../applications/services/pd \
   pd_protocol_test.c ../applications/services/pd/pd_protocol.c -o pd_protocol_test
./pd_protocol_test
```

Expected output: `All PD protocol tests passed.`
