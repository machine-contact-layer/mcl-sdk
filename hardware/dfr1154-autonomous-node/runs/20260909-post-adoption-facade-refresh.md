# DFR1154 post-adoption-facade refresh — 2026-09-09

This is an implementation receipt, not new acoustic evidence.

The autonomous node was rebuilt after the public machine facade gained an
explicit asynchronous-candidate-refusal path. The build staged the canonical
sources from the current repositories and produced:

```text
application bytes  1,268,720
application SHA-256 7156786FC454C2FD275E3F26FCBA3CD693C7D2225A788441455386E75247B36F
static DRAM         241,392 bytes
DRAM headroom        86,288 bytes
```

COM3 was positively identified as ESP32-S3 USB Serial/JTAG
(`VID 303A`, `PID 1001`). `flash-app-only.ps1` wrote and verified only the
application partition beginning at `0x20000`; the bootloader, partition table,
NVS, and retained factory application were not written.

The post-reset serial status was:

```text
MCLAUTO STATUS state=IDLE phase=CONTROL scenario=0 free_heap=22336
largest=13812 psram=8111120/8388608 queue=0/0 wifi=1 ble=0 role=NONE
source_ref=4053881274
```

The board therefore builds, flashes, boots, and remains idle on the updated
facade. The prior continuous positive-receive results remain bound to their
named image hash in `20260909-continuous-positive-receive.md`. No sound was
emitted and no receive trial was run for this refresh. The next evidence event
is the physical DFR1154/Android zero-prior run using the updated endpoints.
