# MCL dual-transport peer (ESP32-S3)

One MCL contact, two live radios, and a real migration between them.

The single-transport rigs in `mcl-ip/hardware/esp32-udp-peer` and
`mcl-ble/hardware/esp32-gatt-peer` each prove that a binding carries Link frames
over a real medium. Neither can prove migration, because migration is the
property that a contact **survives** a change of medium — and a peer with one
radio has no medium to change to.

This sketch brings up a 2.4 GHz SoftAP with a UDP socket **and** a BLE GATT
server, simultaneously, and holds a single `mcl_node_t` across both. The host
harness in [`tools/dual-transport-peer`](../../tools/dual-transport-peer) drives
the sequence; the board is the responder, because somebody has to be and the
deployment owns that choice (charter 2.10.1).

## What makes this expressible

The SDK's transmit callback is told which bearer each frame must leave on, and
the SDK derives that from the contact state:

```text
TRANSPORT_OFFER / ACCEPT        old transport
PATH_CHALLENGE / PATH_RESPONSE  candidate transport
COMMIT / CONFIRM                candidate transport
ordinary traffic                depends on the cutover state
```

Before that parameter existed this sketch could not have been written correctly.
A node had exactly one way out, so "this `PATH_RESPONSE` must go out over IP, not
over the BLE link it was negotiated on" was something the integrator had to infer
from the order of calls.

## What this measures, and what it does not

It measures the four handoff controls crossing real radios and being decoded by
the peer's own codec; a control arriving on the **wrong** transport being
refused; contact continuity — same session reference, same peer reference —
after the medium underneath changes; and repair by retransmission when a `COMMIT`
or a `CONFIRM` is lost.

The wrong-transport case is the one a single-transport rig cannot construct at
all, and it is the check path validation depends on.

It does **not** demonstrate independent interoperability. Both ends compile the
same sources, so a shared misreading of the specification would pass on both
sides and be invisible here. C4 / E6 needs a second implementation written from
the specification by someone else, and nothing in this directory substitutes for
that.

It demonstrates **no security property**. The BLE link uses Just Works pairing,
the access point a shared password, and every MCL reference crosses both media in
the clear. A listener that heard the first contact can complete the whole
migration sequence and be accepted exactly as an honest peer would.

## The firmware is not a copy of the protocol

`build-firmware.ps1` stages the canonical sources out of the sibling
repositories into a throwaway build tree and deletes it afterwards. No protocol
source is duplicated into this directory, so the firmware and the host cannot
drift apart, and the script prints the SHA-256 of every source it staged so a run
can be tied to exact inputs.

The sketch supplies only the radios, the sockets and the serial log. The host
harness supplies only WinRT, sockets and timing: it calls into
`mcl_dual_host.dll`, built from the same sources, through
[`tools/dual_host_shim.c`](../../tools/dual_host_shim.c), which adds no protocol
logic of its own.

## Running it

```powershell
# 1. Build. Compiles only; never uploads.
.\build-firmware.ps1

# 2. Write the application partition, and nothing else.
.\flash-app-only.ps1 -PortName COM3

# 3. Build the shim and the harness.
cmake -S ..\.. -B <build> -DMCL_SDK_BUILD_TOOLS=ON
dotnet build ..\..\tools\dual-transport-peer -c Release -o <out>
#    then place mcl_dual_host.dll beside the harness executable

# 4. Join the board's access point on the interface you intend to use.
netsh wlan connect name="MCL-DUAL-TEST" interface="Wi-Fi 2"

# 5. Run. The host's Bluetooth radio must be on and the board advertising.
mcl_dual_transport_peer.exe --bind 192.168.4.2 --peer 192.168.4.1 `
                           --serial COM3 --cycles 100 --log board-serial.log
```

`--bind` is not optional in practice. Without it the routing table chooses the
interface, and on a machine with more than one radio the traffic may leave by the
wrong one, which produces a result describing a different link from the one being
measured.

The first run pairs the board, which Windows requires before it will open a GATT
session. A device object obtained before pairing keeps failing afterwards, so the
harness re-acquires it.

## Flashing safety

Only the application partition at `0x20000` is written. The bootloader at `0x0`
and the partition table at `0x8000` are untouched, which is what makes the
operation reversible from an application-only backup.

`flash-app-only.ps1` refuses to run unless a factory application backup exists,
so a board cannot be put into a state it cannot be returned from. It also
verifies that the port really is the ESP32-S3 native USB device before writing.

Uploading through the Arduino toolchain is deliberately not used: its upload step
can also rewrite the bootloader and the partition table, which would make the
factory application unrecoverable from that backup.

## Serial interface

| Command | Response |
|---|---|
| `PING` | `MCLPONG` |
| `STATUS` | one line of contact, transport and counter state |
| `RESET` | return to a fresh contact on BLE and zero the counters |
| `DROPNEXT` | discard the next frame this board would transmit |

`DROPNEXT` reports the frame as accepted and then discards it, which is the case
that matters: a transmit failure the sender can see is easy to handle, and a
frame that vanishes after a successful send is what actually happens on a radio.
It is how the host tests a lost `CONFIRM` without unplugging a radio.

Every event produces one `MCLDUAL` line, which is the per-run evidence record.
The board's own counters are the ground truth for what it received: a host that
reports a failure while the board reports a clean refusal is describing a lost
frame, not a protocol defect, and the two records have to be read together.

## Two rules about the serial port, both learned the hard way

**Event logging is guarded and drops rather than blocks.** `Serial.printf` blocks
once the USB CDC transmit buffer fills, which it does immediately if no host is
reading, and while it blocks the radios back up. Every event line is guarded by
`Serial.availableForWrite()`; a line is dropped and counted instead, and `STATUS`
reports `logdrop` so a quiet run is never mistaken for a clean one.

**Command answers are bounded, not guarded.** A dropped `STATUS` is not a lost
log line, it is the run's second record going missing — but it must not block
either. `serial_emit` writes in chunks with a deadline that resets on progress, so
a slow reader is tolerated and an absent one is not. This is not hypothetical:
with plain `Serial.printf` answers, a host that closed the port mid-write left
the board blocked in that write forever. It went on answering ICMP from the
network stack while its `loop()`, and therefore both MCL radios, were dead —
alive on the radio, deaf on the protocol, with nothing in the log to say so.

A host reading this port must read it continuously. See the note on the reader
thread in the evidence README.

## Evidence

[`mcl-sdk/evidence/e4-dual-transport-migration-20260903`](../../evidence/e4-dual-transport-migration-20260903)
— 104 migrations, 2989 checks, 0 failed, and the two protocol defects the run
found.
