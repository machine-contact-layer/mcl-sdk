# DFR1154 BLE memory feasibility spike

**LAB INSTRUMENT.** Exchanges nothing, proves nothing about the protocol, and
produces no MCL evidence. It answers one question that had to be settled before
the autonomous node was wired for a second radio:

> Can a BLE stack coexist, on this board, with the static allocations the node
> already makes — and if it can, how much room is left?

## Why a spike rather than "add BLE and see"

Two different failures both look like *BLE does not work here*, and only one of
them can be worked around at runtime:

| | what it looks like | can Wi-Fi quiescing fix it? |
|---|---|---|
| **static** | `.dram0.bss will not fit in region dram0_0_seg` | **no** — the memory was never there |
| **runtime** | `BLEDevice::init()` returns false, or a connection cannot be allocated | **yes** — Wi-Fi is holding heap BLE wants |

The node's `quiesce_wifi` option existed for an evidence reason: a control plane
on the air during an exchange contaminates the result. This spike asked whether
it is also a memory requirement. It is.

## What was measured

Three builds, each flashed to the board and read back over the USB CDC port.
Raw captures are in [`runs/`](runs/), with the image hash each came from.

### 1. The node's layout as it stood — does not link

```
arena  66 560 samples  133 120 bytes   the modulated 17-byte waveform
scratch                 76 868 bytes   the modem's correlation working set
                       ----------
                       209 988 bytes  in .bss, plus Wi-Fi, plus Bluedroid

ld: section `.dram0.bss' will not fit in region `dram0_0_seg'
```

Measured either side of the failure: everything except the audio buffers costs
**50 268 bytes** of static RAM, and the largest arena that links beside the
scratch is between 61 440 and 66 560 samples. So the deficit is small — about
10 KB — which is why the remedy is a layout change and not a redesign.

### 2. One union arena — links, and is what the node now uses

Transmit and reception never overlap: while this machine's speaker is driven,
its microphone hears its own emission and nothing else. The window and the
scratch **are** both live during reception; the waveform is live only during
transmission. So the block is sized by the larger of the two uses, not by their
sum:

```
receive     window 94 720 + scratch 76 868   = 171 588
transmit    waveform                           133 120
                                              ---------
arena       max(...)                           171 588 bytes   (saves 38 400)

Global variables use 221820 bytes (67%), leaving 105860 bytes.
```

Runtime, from [`runs/01-static-union-arena.log`](runs/01-static-union-arena.log):

```
boot                        free 113 468
Wi-Fi SoftAP + HTTP         free  54 796
BLE init, Wi-Fi UP          REFUSED
Wi-Fi down                  free  92 156     (~21 KB is not returned)
BLE init, Wi-Fi DOWN        free  21 832
+ GATT server               free  18 748
+ advertising               free  17 816
+ scanning                  free  17 584
+ client object             free  16 828     largest block 8 692
```

**Two results the node is built on.** BLE and Wi-Fi cannot both be up in this
layout — the refusal is a memory shortage, not a radio conflict, and run 2
proves that by bringing them up together when the arena is elsewhere. And
`esp_wifi_deinit()` after `WiFi.mode(WIFI_OFF)` returns `ESP_ERR_WIFI_NOT_INIT`:
the driver is already gone, and the ~21 KB Wi-Fi does not give back belongs to
the network stack below it, so there is nothing further to reclaim.

### 3. The arena on the heap — better, and unusable

Taking the same 171 588 bytes from the heap and releasing it before the radios
come up gives a great deal of room: BLE comes up **with Wi-Fi still running**,
and everything up to a client object leaves 151 928 bytes free.

Then the node has to listen again, and cannot:

```
after BLE                      free 190 612   largest 110 580   re-acquire REFUSED
window alone (94 720)          ok
  scratch alone (76 868)       REFUSED
after BLEDevice::deinit(true)  free 265 808   largest 112 628   re-acquire REFUSED
```

The heap is permanently split by the radio stacks' own allocations, and tearing
BLE down completely does not repair it. **265 KB free and no 168 KB block in
it.** A node that can listen once and never again is not a node, so this layout
was rejected — and it was rejected on the return trip, which is exactly the
measurement a spike is for.

## The verdict the node is built on

```
ONE STATIC UNION ARENA          171 588 bytes, allocated before anything can
                                fragment, therefore always available

WI-FI AND BLE ARE EXCLUSIVE     not a preference. Measured: BLE init is refused
                                with Wi-Fi up in this layout

THE BEARER PHASE RUNS WITH      ~16.8 KB free heap, largest block ~8.7 KB
WI-FI DOWN
```

The last line is tight and is stated as tight. It is enough to bring up a GATT
server, an advertiser, a scanner and a client object simultaneously, which is
more than one role ever needs at once: `BLE-ACTIVATE-1` derives the roles from
the wire, so an offerer advertises and serves while an acceptor scans and
connects, and neither builds the other half.

If a real connection proves not to fit in it, the next remedy is already
identified and needs no protocol change: **never bring Wi-Fi up at all** in a
BLE run, arming over the serial port instead, which the run-1 numbers say is
worth about 21 KB more.

## Repeating the measurement

```powershell
.\build-firmware.ps1 -Sketch spike -Defines '-DSPIKE_ARENA_SAMPLES=85794 -DSPIKE_WITH_SCRATCH=0'
.\flash-app-only.ps1 -Image .\build\spike-out\mcl-ble-spike-staging.ino.bin
```

`SPIKE_ARENA_SAMPLES`, `SPIKE_WITH_SCRATCH` and `SPIKE_HEAP_ARENA` select the
layout. The sketch prints `SPIKE STATIC`, one `SPIKE STAGE` line per stage in
order, and one `SPIKE RESULT`.

**One trap, recorded because it nearly published a wrong number.** The first
version of the sketch declared the modem scratch and never used it; the linker
removed it and reported 181 892 bytes of global variables against the node's
real 271 680. Every static is now touched through a `volatile` pointer.
