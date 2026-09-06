# DFR1154 autonomous MCL node

**LAB / EXPERIMENTAL.** A test node, not a product, and not a second
implementation of anything.

This is the board that has to answer release gate row 35's remaining question:
can two machines that were told *nothing* about each other find each other,
agree on a bearer, reach it, validate it and migrate — with no host in the loop
making the decisions that are supposed to be the protocol's?

## Why a third rig exists

Two already do, and neither can reach that question.

| rig | runs MCL on the board | hears anything | decides anything |
|---|---|---|---|
| `mcl-ap/experiments/008-embedded-node` | yes — Wire, Link, AP modem | yes | **no** — the laptop sends `SEND WIRE`, `LISTEN FRAME`, and picks the moment |
| `mcl-sdk/hardware/esp32-dual-peer` | yes — one contact across live UDP and BLE, and it migrates | **no microphone at all** | no — the host drives the sequence |
| this node | yes | yes | **yes** — it owns its clock, randomness, radios and rendezvous state |

A node that waits for a serial command cannot demonstrate autonomous first
contact, because the command is the answer.

## The rule that makes the evidence worth anything

> **The lab control plane is not the MCL data plane.**

The node exposes an HTTP API over its own SoftAP so a scenario can be armed and
a log retrieved without a cable. That is *instrumentation*. It would be very
easy — and completely invisible in a result — to let it also carry a peer
address, a token or a session reference, and then report "two strangers
discovered each other" about a run where one of them was told the answer.

Two things stop that, and only the second is real:

1. **The configuration surface has no field for peer-specific data.** There is
   nowhere to put a peer IP, UDP port, BLE address, `source_ref`,
   `endpoint_token`, `migration_ref`, `session_ref`, secret or pairing state.
   A good fence, and only as strong as the next person to edit the struct.

2. **Every peer-identifying value is logged with its provenance** — `LOCAL`,
   `FROM_AIR`, `FROM_BEARER` or `CONFIGURED`. A run is zero-prior **if and only
   if** no value reads `CONFIGURED`, and `/api/result` computes `zero_prior`
   from the tags rather than asserting it. Someone who does not trust this
   README can check it from the log.

`CONFIGURED` is a value this firmware never writes. It exists so that if a
configuration path for one of these values is ever added, every run says so
instead of silently becoming untrue.

## Memory, which decided the design

Measured on this tree, not estimated:

```
AP listener window, 17-byte max payload    47 360 samples    92 KB
modem receive scratch                      76 868 bytes      75 KB
modulating a 17-byte object                66 560 samples   130 KB
```

Those, plus a Wi-Fi stack, plus a BLE stack, do not fit in roughly 320 KB of
internal DRAM. The built image confirms it:

```
Global variables use 271680 bytes (82%) of dynamic memory,
leaving 56000 bytes for local variables. Maximum is 327680 bytes.
```

That is **with Wi-Fi and the HTTP server, and without BLE**. Two consequences,
and the second is the interesting one:

- **One arena, not two buffers.** Transmit and receive never overlap — while
  this machine's speaker is driven its microphone hears its own emission and
  nothing else, which is why `mcl_rdv_platform_t` has `self_transmitting` at
  all. So one 130 KB arena is the listener's window and is repurposed for
  modulation around a transmission, with the listener reset either side. The
  cost is any frame half-buffered when we start talking, which the physics was
  going to take anyway.

- **Quiescing Wi-Fi during a run is not tidiness, it is the memory budget.**
  The `quiesce_wifi` option was added because a control plane on the air during
  an exchange contaminates the evidence. The measurement above says it is also
  how BLE gets enough DRAM to come up at all. The honesty requirement and the
  physical constraint happen to want the same thing.

**PSRAM is not used.** The board has 8 MB and it is the obvious way out.
Experiment 008 declined it for a reason that still holds: the correlation inner
loop reads the window tens of millions of times per acquisition, and this
project has never verified the QSPI/OPI mode option for this part. A rig that
boots differently depending on a board option nobody checked is not an
instrument.

## HTTP API

Reachable on the node's SoftAP (`mcl-auto-node`). Deliberately small — no
framework, no bundle, no camera, no TLS.

```
GET  /api/status     node, majors, run state, heap, arena, log depth
POST /api/config     scenario, duration_ms, band_low_hz, band_high_hz,
                     emit_gain_pct, quiesce_wifi        (no peer fields)
POST /api/run        arm and start
POST /api/stop       stop
GET  /api/result     counters, failure, values[] with provenance, zero_prior
GET  /api/log        NDJSON, one record per line
```

The log is a fixed ring with an explicit `log_dropped` counter: a log that
silently discards its oldest entries turns a run that overflowed it into a run
that looks complete.

## Scenarios

| id | name | emits sound |
|---|---|---|
| 0 | listen only — reports QUIET / HEARD / CONTACT distinctly | **no** |
| 1 | announce and listen — PRESENCE at a randomised cadence | **yes** |

Scenario 0 establishes the room's own noise before anything is concluded from a
failure, and is safe to run at any time. Scenario 1 is audible: it is a
campaign, and campaigns are approved and budgeted before they run.

## Band changes move the whole waveform

`apply_band()` is byte-for-byte experiment 008's convention — tones **and** the
preamble chirp, derived as `f0-1000 .. f1` with a 500 Hz floor. Two rigs that
derive the chirp differently are not measuring the same band.

Experiment 011 found the other half of that trap: a rig that applied a band on
the host and never sent it to the board reported about a *different band* at a
perfectly healthy signal level. Here there is no host to disagree with — the
node applies the band to its own transmitter and its own receiver from one
function, and the run log records it.

## What this node cannot establish

- **Independent interoperability.** Both ends of any run compile the same
  sources. A shared misreading of the specification passes on both sides and is
  invisible. Nothing here substitutes for a second implementation.
- **Security.** There is none. The SoftAP has a password and any BLE link would
  use Just Works; every MCL reference crosses in the clear. A listener that
  heard first contact can complete the sequence and be accepted exactly as an
  honest peer would. That is the honest limit of correlation and reachability
  without cryptography, and MCL v1 claims nothing else.
- **Stranger discovery over IP.** Any UDP endpoint used here is harness
  configuration and is recorded as such. MCL defines no global discovery port
  and this rig does not invent one.

## Build and flash

```powershell
.\build-firmware.ps1          # builds only; refuses without a factory backup
.\flash-app-only.ps1          # writes 0x20000 only; refuses without a backup
```

Flashing is a decision, not a build step. The application partition is written
at `0x20000` with esptool; the bootloader, partition table and NVS are left
untouched, which is what makes it reversible. Restore with
`RESTORE_DFR1154_APP.cmd`.

## Status

The firmware builds and the memory budget above is measured from that build.
The rendezvous coordinator, the BLE-ACTIVATE candidate hooks and the MCL-IP
carriage scenario are **not yet wired in** — the arena and radio budget had to
be settled first, because it decides whether they can coexist, and it does not
allow Wi-Fi and BLE up together with the audio buffers allocated.
