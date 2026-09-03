# One contact, two live radios, 104 migrations — 2026-09-03

First execution of an MCL transport migration between two machines over real
radios, with **both media alive at the same time on both peers**.

Every over-air run before this one exercised a single binding over a single
medium. That establishes carriage. It cannot establish migration, because
migration is the property that a contact **survives** a change of medium, and a
peer with one radio has no medium to change to.

## Claim

**Conformance:** C3 (contact state machine) exercised across a physical
transport change, plus the handoff control encoding of C1/C2, demonstrated
between two machines.

**Evidence level:** `E4 MULTI_DEVICE_OVER_AIR`.

**Explicitly not claimed:** C4 or `E6 INDEPENDENT_INTEROPERABILITY`. Both ends
compile the same `mcl-sdk`, `mcl-link`, `mcl-wire`, `mcl-ip` and `mcl-ble`
sources. A shared misreading of the specification would be accepted by both
peers and would not show up anywhere in this result. That gap closes only with a
second implementation written from the specification by someone else.

**No security property whatever is claimed.** The BLE link uses Just Works
pairing, the access point a shared password, and every MCL reference in the
exchange — `source_ref`, `migration_ref`, `session_ref`, `endpoint_token` —
crossed both media in the clear. A listener in range of the first contact could
have completed this entire migration sequence and been accepted exactly as the
honest peer was. That is the honest limit of correlation and reachability
without cryptography, and it is what
`mcl-link/spec/link-handoff-control-v0.1.md` §4.2 already says.

## Setup

| | |
|---|---|
| Peer A (host) | Windows laptop; MediaTek Bluetooth LE; Realtek RTL8188EU 802.11n USB adapter |
| Peer B (board) | DFR1154 / ESP32-S3, `mcl-sdk/hardware/esp32-dual-peer` |
| BLE path | GATT, fragmented at ATT MTU 23 — the smallest BLE permits |
| IP path | 2.4 GHz SoftAP `MCL-DUAL-TEST` ch 6, WPA2-AES, UDP 5555, board 192.168.4.1, host 192.168.4.2 |
| Host binary | `mcl_dual_transport_peer`, `mcl_dual_host.dll` built with `MCL_SDK_BUILD_TOOLS=ON` |
| Firmware | app partition only at `0x20000`, SHA-256 `E8349132CA26218C772684F943CC2D3AF410D22978FEB037A05A4205C2444332` |

Both radios were up on both peers for the whole run. On the ESP32-S3 the Wi-Fi
and BLE radios share one 2.4 GHz front end and coexist by time-slicing, so that
constraint is part of what was measured rather than something the rig avoided.

The host's UDP socket was bound to 192.168.4.2 explicitly. Without that the
routing table picks the interface, and the result then describes a different
link from the one being measured.

## Result

```text
2989 checks, 0 failed

104 migrations completed (4 named cases + 100 consecutive cycles)
100 consecutive alternating BLE<->IP migrations in 14.2 s, 100 of 100 completed

board: rx_ble=167 rx_ip=161 tx_ble=161 tx_ip=160 rej=5 wrongxp=2 drop=1 logdrop=0
host:  BLE frames sent=167, fragments received=322, reassembly refusals=0
       UDP datagrams sent=161, received=160, refused by the binding=0
```

**Host and board records agree exactly.** 167 BLE frames sent, 167 received.
161 datagrams sent, 161 received. 160 datagram replies sent, 160 received. No
loss in either direction on either medium, and `logdrop=0`, so the board's
serial record is complete rather than merely quiet.

One session reference, `535F0193`, and one peer reference, `0D0A1154`, survived
all 104 changes of medium. That is the property this experiment exists to test.

## What was refused, and why each one matters

| Case | Result |
|---|---|
| A valid frame replayed on a transport the contact does not live on | `WRONG_TRANSPORT`, no reply |
| A valid `PATH_CHALLENGE` delivered on the OLD path during validation | `WRONG_TRANSPORT`, no `PATH_RESPONSE` |
| A duplicate `TRANSPORT_OFFER` after the acceptance | refused, contact stayed `AGREED` |
| A `CONFIRM` naming a migration that does not exist | decoded, refused on apply, contact unmoved |
| A truncated handoff control payload | refused by the control decoder |
| An unassigned handoff operation | refused by the control decoder |
| An offer naming reserved profile 0 | refused before reaching a radio |
| An offer naming the transport already in use | refused: adaptation is not migration |
| An offer with migration reference 0 | refused: it names no transaction |

**The first two are the reason this rig had to exist.** A control that is
correct in every reference, delivered over the wrong medium, is the one check
path validation depends on, and a single-transport rig cannot construct it at
all. Accepting it would declare a candidate path reachable on the strength of
bytes that never crossed it — which is precisely, and only, what
`PATH_CHALLENGE`/`PATH_RESPONSE` exist to establish.

The board's counters separate the two cases the host cannot tell apart on its
own: `rx_ip` rose, so the radio did receive the datagram, and `wrongxp` rose, so
it was refused rather than lost.

## Repair by retransmission

Two migrations were run with a frame deliberately destroyed after a successful
send, which is what actually happens on a radio — a transmit failure the sender
can see is the easy case.

- **Host's `COMMIT` lost.** The host's transmit callback reported the frame
  accepted and discarded it. The contact entered `COMMITTING` correctly, because
  from the sender's side a commit that may have left is irrevocable.
- **Board's `CONFIRM` lost.** The board's `DROPNEXT` discarded its next
  transmission after applying the commit, so the board had migrated and the host
  did not know.

Both were repaired by retransmitting the `COMMIT`, and one retransmission covers
both losses: if the commit was lost the peer acts on it now, and if the confirm
was lost the peer is already `ACTIVE` and re-confirms. The sender cannot tell
which happened and does not need to.

The duplicate `PATH_CHALLENGE` row was exercised over the air for the same
reason: the board re-echoed the identical challenge from `VALIDATED` rather than
refusing it, which is what stops one dropped response from killing a migration
with both peers behaving correctly.

## Two defects this run found

Neither would have appeared in a loopback test, and both are fixed in the
commits this evidence accompanies.

**1. The SDK could not address a frame.** Every send path honoured
`MCL_LINK_FLAG_DESTINATION` from the caller's flags and then set
`destination_ref` to zero unconditionally. An "addressed" frame was addressed to
reference zero — to nobody — which a correct receiver refuses as
`NOT_ADDRESSED`. The sending half of the addressing rule simply did not exist,
so on a bearer two machines share, a node could not direct a frame at its peer
at all. Fixed in `mcl_node_fill_destination`, with a regression test.

**2. Reserved profile 0 was accepted.** All four transport profile registries
state that zero is permanently reserved and that an offer carrying it is
malformed. Nothing anywhere enforced it. Fixed in `mcl_contact_record_offer` and
`mcl_contact_resolve_offer_collision`, beside the identical rule for transport 0.

## Two instrument traps, neither a protocol defect

Recorded because both produced results that looked like firmware failures.

**The host's serial reader stopped, and took the board's evidence with it.** The
first version of the harness read the board's console through
`SerialPort.DataReceived`. It stopped firing partway through a hundred
migrations. The board's logging is guarded by `Serial.availableForWrite()` and
drops a line rather than blocking, so a host that stops draining the port
silently switches the board's entire record off — the log went quiet from cycle
40 onward while the run continued perfectly. Replaced with a dedicated blocking
reader thread; `logdrop=0` in this run is the evidence it worked.

**An unguarded console write wedged the board.** The board's `STATUS`, `PING`
and `RESET` answers were plain `Serial.printf` calls. When the harness closed
the port mid-write, the board blocked in that write forever — and went on
answering ICMP from the network stack while its `loop()`, and therefore both MCL
radios, were dead. Alive on the radio, deaf on the protocol, with nothing in the
log to say so. It needed a hard reset and looked exactly like a crash. Every
write in the sketch now goes through `serial_emit`, which waits with a deadline
that resets on progress: a slow reader is tolerated, an absent one is not.

## What this does not cover

Stated so the gaps are not mistaken for results.

- **Simultaneous offers and the exact tie.** The board is the responder and
  never initiates, so glare cannot arise on this rig. Covered by
  `mcl-link/tests/test_contact.c` only.
- **A wrong challenge echo.** Both ends are correct implementations, and neither
  can be made to lie without putting a lie in the shipped code. Covered by unit
  tests only.
- **Endpoint resolution failure.** `endpoint_token` is a rendezvous reference,
  and on UDP the board resolves it as "reply where the probe came from", so
  there is no lookup here that can fail.
- **Anything about identity, authority or trust.** See the claim above.

## Reproducing

```powershell
cd mcl-sdk\hardware\esp32-dual-peer
.\build-firmware.ps1
.\flash-app-only.ps1 -PortName COM3

cmake -S ..\.. -B <build> -DMCL_SDK_BUILD_TOOLS=ON
dotnet build ..\..\tools\dual-transport-peer -c Release -o <out>
#   then place mcl_dual_host.dll beside the harness executable

netsh wlan connect name="MCL-DUAL-TEST" interface="Wi-Fi 2"
mcl_dual_transport_peer.exe --bind 192.168.4.2 --peer 192.168.4.1 `
                           --serial COM3 --cycles 100 --log board-serial.log
```

The host machine's Bluetooth radio must be on, and the board must be advertising.
`--bind` is not optional in practice: see the note on interface selection above.

## Files

- `host-output.txt` — the harness's own record, 2989 checks.
- `board-serial.log` — every event the board logged, interleaved with the
  harness's step markers. The board's counters are the ground truth for what it
  received; a host reporting a failure while the board reports a clean refusal
  is describing a lost frame, not a protocol defect, and the two records only
  settle that question when read together.
