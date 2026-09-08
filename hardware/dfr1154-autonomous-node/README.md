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

Measured on this tree, not estimated, and measured again with a BLE stack
linked in. The full campaign is in [`spike-ble-memory/`](spike-ble-memory/);
three results shape this firmware.

**One union arena, not two buffers.** The receive path needs the listener's
window *and* the modem's scratch at the same time. The transmit path needs
neither -- only a waveform. Sized separately they are 209 988 bytes and the
image does not link with a BLE stack:

```
ld: section `.dram0.bss' will not fit in region `dram0_0_seg'
```

Sized as one block by the larger **use** rather than the sum, they fit:

```
receiving     window 94 720 + modem scratch 76 868  =  171 588   <- the larger
transmitting  waveform for a 17-byte object         =  133 120

arena                                                  171 588 bytes
```

Transmit and reception never overlap -- while this machine's speaker is driven
its microphone hears its own emission and nothing else, which is why
`mcl_rdv_platform_t` has `self_transmitting` at all -- so the waveform is
allowed to sit on top of both, with the listener reset either side. The cost is
any frame half-buffered when we start talking, which the physics was going to
take anyway.

**The arena is static, and the comfortable option was measured and rejected.**
On the heap it is far roomier: BLE even comes up with Wi-Fi still running.
Then the node has to listen again and cannot -- after the radios have run,
265 KB free contains no 168 KB block, and tearing the whole BLE stack down does
not repair it. A node that can listen once and never again is not a node.

**Wi-Fi and BLE are exclusive here, and Wi-Fi does not give back what it
takes.** `BLEDevice::init()` is *refused* with the SoftAP up in this layout.
And a shutdown returns only part of the heap: 113 468 bytes free at boot,
54 796 with the SoftAP up, 92 156 after it is taken down -- about 21 KB stays
with the network stack underneath the driver, and `esp_wifi_deinit()` answers
`ESP_ERR_WIFI_NOT_INIT` because the driver has already gone.

That 21 KB is the difference between having a BLE phase and not having one, so
**arming restarts the board**: the configuration goes to RTC memory and the node
boots into the run with Wi-Fi never initialised. Every run therefore has the
same memory state whichever control plane armed it. The alternative -- "arm
over serial for BLE runs and over HTTP for the rest" -- makes the result depend
on the instrument, which is the class of mistake this rig exists to avoid.

Measured on the shipping firmware, holding a live GATT connection:

```
static (link time)                 238 580 bytes, 72% of DRAM
boot, Wi-Fi never started           87 840 free
BLE up                              16 124 free    largest 8 180
advertising                         12 116 free    largest 7 668
connection + a 3-fragment frame in and out         rc=0
```

**PSRAM is used as an audio queue, not as decoder working memory.** OPI mode was
verified on this exact module: the node reports 8,388,608 bytes and retained
about 8.11 MB free at boot. A core-0 I2S producer writes a 131,072-sample SPSC
queue there while the correlation window and scratch remain in internal DRAM.
This separates continuous capture from variable positive-decode latency without
putting the hot correlation working set behind PSRAM. The measured positive and
quiet receipts are in
[`runs/20260909-continuous-positive-receive.md`](runs/20260909-continuous-positive-receive.md).

## Phases

Two radios that cannot be up together make "which one is up now"
protocol-visible rather than bookkeeping, so it is a state:

```
CONTROL     Wi-Fi + HTTP up. Armed from here. No MCL traffic.
QUIESCE     Wi-Fi down, before either radio is asked for anything.
RENDEZVOUS  AP-BOOTSTRAP-1: PRESENCE, contention, OFFER / ACCEPT.
ACTIVATE    BLE-ACTIVATE-1: the offerer advertises, the acceptor scans.
VALIDATE    PATH_CHALLENGE / PATH_RESPONSE on the candidate.
POLICY      admit or refuse, before the first irrevocable act on each side.
MIGRATE     COMMIT / CONFIRM.
TEARDOWN    radios down.
REPORT      Wi-Fi + HTTP restored, result readable.
```

`ACTIVATE` does not immediately retire the bootstrap receiver. Until the
candidate has actually carried traffic, a lost acoustic `TRANSPORT_ACCEPT`
must still recover: the offerer repeats the same `TRANSPORT_OFFER`, and the
acceptor hears it and re-emits the stored acceptance. AP listening therefore
remains live through `ACTIVATE` and stands down only at validation/policy.

## Roles are read off the wire

`BLE-ACTIVATE-1` section 2, implemented literally. Only `TRANSPORT_OFFER`
carries an `endpoint_token` and it is the offerer's own, so the offerer is the
only peer that can be *found*:

| peer | knows | therefore |
|---|---|---|
| offerer | its own token | **advertises**, GATT peripheral |
| acceptor | the offerer's token | **scans and connects**, GATT central |

Nothing in this firmware chooses a role. `MCL_RDV_EVENT_BEARER_AGREED` carries
`peer_endpoint_token`, which is non-zero exactly for the peer that received an
offer, and that is the discriminator.

The scanner matches on the service UUID **and** the full eight-byte beacon, by
walking the raw advertising payload rather than asking the library for
"service data" -- and `/api/scan-record` returns those bytes, so "the two
implementations encode the same AD structure" is checkable instead of asserted.
An advertisement matching the UUID but not the beacon is counted separately as
`scan_uuid_only`: right protocol, wrong transaction.

## HTTP API

Reachable on the node's SoftAP (`mcl-auto-node`). Deliberately small — no
framework, no bundle, no camera, no TLS.

```
GET  /api/status       node, majors, run state, phase, heap, radios, log depth
POST /api/config       scenario, duration_ms, band_low_hz, band_high_hz,
                       emit_gain_pct, quiesce_wifi, candidate_transport,
                       admit_policy                       (no peer fields)
POST /api/run          arm; the node RESTARTS into the run
POST /api/stop         stop
GET  /api/result       counters, failure, values[] with provenance, zero_prior
GET  /api/scan-record  the raw advertising payload the scanner matched
GET  /api/log          NDJSON, one record per line
```

`candidate_transport` names a **medium**, not a peer: 3 for BLE, 2 for IP.
Which bearer a deployment offers as its continuation is a deployment-profile
decision and says nothing about who is on the other end.

## Serial control plane

Same standing as HTTP and the same fence. Useful for a board on a cable, and
for watching a run as it happens rather than polling it.

```
CONFIG <scenario> <duration_ms> <band_low> <band_high> <gain> <candidate> <admit>
ARM            arm; the node restarts into the run
STOP           stop a run
STATUS         state, phase, heap, which radios are up
RESULT         counters, and every tagged value with its provenance
LOG            the whole ring
BLETEST <1|2>  diagnostic: bring BLE up as advertiser (1) or scanner (2)
BLEDOWN        tear it down again
WIFI ON | WIFI OFF
```

[`node-serial.ps1`](node-serial.ps1) drives it:

```powershell
.\node-serial.ps1 -Reset -Command 'CONFIG 2 120000 0 0 100 3 1' -Then ARM -Listen 130
.\node-serial.ps1 -Command RESULT
```

The log is a fixed ring with an explicit `log_dropped` counter: a log that
silently discards its oldest entries turns a run that overflowed it into a run
that looks complete.

## Scenarios

| id | name | emits sound | radios |
|---|---|---|---|
| 0 | listen only — reports QUIET / HEARD / CONTACT distinctly | **no** | mic |
| 1 | announce and listen — PRESENCE at a randomised cadence | **yes** | mic + speaker |
| 2 | **zero-prior rendezvous** — the whole path, through migration | **yes** | mic + speaker + BLE |
| 3 | MCL-IP carriage, as a responder on a port of its own | no | Wi-Fi |
| 4 | BLE-ACTIVATE diagnostic: advertise a compiled-in token | no | BLE |
| 5 | BLE-ACTIVATE diagnostic: scan for that token | no | BLE |

Scenarios 4 and 5 are **not zero-prior and report themselves as such**: the
token is a constant in the firmware rather than a value learned from the air,
so it is tagged `CONFIGURED` and `/api/result` says `zero_prior: false`. They
exist to check that two implementations encode the same 26 advertising bytes,
which is a different question from whether two strangers found each other.

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

The rendezvous coordinator, the `BLE-ACTIVATE-1` roles, `BLE-GATT-1` carriage,
the candidate callbacks, local policy and the MCL-IP scenario are wired in, and
the firmware runs the phase machine above with nothing attached.

Verified on hardware so far:

- the image links and boots at 238 580 bytes of static RAM;
- an armed run restarts into itself with Wi-Fi never initialised, and the BLE
  stack comes up in that state;
- the advertisement is exactly `BLE-ACTIVATE-1` section 3 — checked from a host
  with no shared code, in
  [`mcl-ble/hardware/host-ble-probe/`](../../../mcl-ble/hardware/host-ble-probe/);
- a 40-byte frame crosses the GATT link as three fragments at the 23-byte
  minimum MTU, in both directions, byte-identical.
- the first Android/DFR zero-prior attempt proved acoustic reception in both
  directions but failed after one lost acceptance exposed a board-adapter
  lifecycle defect; the negative receipt and corrective image are retained in
  [`runs/20260909-android-dfr-zero-prior-attempt-01.md`](runs/20260909-android-dfr-zero-prior-attempt-01.md).

Not established here, and not claimable until it is: a **complete zero-prior
run** — acoustic first contact through BLE activation to `CONTACT_MIGRATED`
— needs a second machine with a microphone, a speaker and a BLE radio. That is
the second builder, not this board.
