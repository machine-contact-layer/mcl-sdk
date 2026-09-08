# MCL v1 reference resource envelope

These are measured build outputs and `sizeof` results, not estimates. Run
`packaging/verify-developer-sdk.sh` to regenerate the portable structure report.
Compiler, alignment, transport stack, and application code change the numbers;
do not treat this table as an ABI promise.

## Portable state, MSVC x64

| component | bytes |
|---|---:|
| Base node, `mcl_node_t` | 136 |
| Rendezvous coordinator, `mcl_rdv_t` | 328 |
| Product facade, `mcl_machine_t` | 600 |
| AP modem scratch | 76,912 |
| AP listener state | 160 |
| AP minimum PCM16 window for the 17-byte bootstrap maximum | 94,720 |
| AP scratch + listener + minimum window | 171,792 |

The Base-only path needs no acoustic scratch or sample window. The Stranger-
Contact reference path does. All storage is caller-owned; there is no hidden
protocol heap allocation.

## DFR1154 / ESP32-S3 reference node

Final application image built 2026-09-09:

| measurement | value |
|---|---:|
| application partition image | 1,268,640 bytes |
| configured application partition | 3,145,728 bytes |
| linked global/static DRAM | 241,392 bytes |
| linker-reported DRAM remaining | 86,288 bytes |
| runtime free internal heap, Wi-Fi never started | 77,228 bytes |
| OPI PSRAM detected | 8,388,608 bytes |
| runtime free PSRAM at boot | about 8.11 MB |
| continuous-capture queue in PSRAM | 262,144 bytes |

The image size includes the Arduino/ESP32 runtime, I2S, Wi-Fi control plane,
BLE, IP, logging, HTTP instrumentation, and the MCL stack. It is deliberately
reported as an inclusive deployable image rather than attributed to MCL by
subtracting unrelated builds with different link-time reachability.

The hot AP correlation window and scratch remain in internal DRAM. Only the
single-producer/single-consumer capture queue is in PSRAM. The positive-traffic
receipt and final quiet control are in
`hardware/dfr1154-autonomous-node/runs/20260909-continuous-positive-receive.md`.

## Qualification rule for a new port

A port must record its own:

- `sizeof(mcl_machine_t)` and any enabled bearer/modem working sets;
- linked flash and static RAM from the final application, with toolchain and
  configuration;
- minimum free heap and largest allocatable block in every radio phase;
- capture queue depth, drops, listener overruns, and unscanned samples under
  quiet and positive traffic;
- worst positive-path `mcl_machine_poll`/decoder service time.

A quiet-room CPU number is insufficient. The receiver must remain available
while it is processing valid MCL traffic.
