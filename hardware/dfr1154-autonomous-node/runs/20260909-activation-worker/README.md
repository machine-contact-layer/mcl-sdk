# DFR activation audit and pre-Android board qualification

Date: 2026-09-09, Asia/Calcutta. **Release not approved.** These are board
component and recovery checks, not a successful zero-prior machine lifecycle.

Application SHA-256:
`F4F1C89BB837E9CAE0696A441138C45BD6FE8D13872AB15C833E8857D02CE8FD`
(1,274,400 bytes). Arduino ESP32 core 3.3.11, ESP32-S3, OPI PSRAM.
`build-manifest.txt` binds the sketch, build script, canonical C sources and
installed BLE library source hashes. Only the application at `0x20000` was
flashed. Bootloader, partitions, NVS and model partitions were not written.

## Defects distinguished from hypotheses

The earlier 16 KB versus 5.4 KB comparison did not prove memory exhaustion as
the cause of the first failed central connection. The former was a
peripheral-role receipt; the supposed reverse-role proof recorded scanning.
The earlier four retries also allocated further clients, so they were not
independent repetitions from the same resource state.

Four adapter defects are addressed:

1. Connection, discovery and subscription ran synchronously on the MCL loop.
   They now run on a persistent worker; the main loop retains AP/MCL service,
   cancels expired work, rejects stale completions and waits for ownership
   release before teardown.
2. Every retry allocated another client. Failed connections now reuse one
   client; stack teardown owns its deletion. A partial discovery failure ends
   the transaction rather than reusing a partially cached peer service tree.
3. The failure branch resumed an unbounded continuing scan. All restart paths
   now use bounded slices with clearing semantics.
4. Native NimBLE address bytes were passed to a constructor that reverses
   them. The `ble_addr_t` constructor preserves both native order and type.
   Boot tests demonstrate the old failure with a non-symmetric address for
   both public and random address types, and require the corrected identity.

The beacon matcher also rejects extra bytes rather than accepting an overlong
service-data field. Boot controls cover exact, truncated, overlong and wrong
token input. The log ring moved to PSRAM to make room for the worker; decoder
window/scratch remain internal. This placement change is not causal proof for
the earlier connection refusal.

The library behavior was checked in the installed sources and the matching
[BLEAddress.cpp](https://github.com/espressif/arduino-esp32/blob/3.3.11/libraries/BLE/src/BLEAddress.cpp),
[BLEClient.cpp](https://github.com/espressif/arduino-esp32/blob/3.3.11/libraries/BLE/src/BLEClient.cpp)
and [BLEDevice.cpp](https://github.com/espressif/arduino-esp32/blob/3.3.11/libraries/BLE/src/BLEDevice.cpp).
Its NimBLE `connect` ignores the public timeout argument. Cancellation uses
the controller API; `gap=-1` in a receipt means unobserved, not a BLE status.

Final review also made the callback connection/mailbox flags atomic and
made run completion wait until the main loop consumed the worker receipt.
The final-image checks below were repeated after both changes. Prior
F6F64E23 receipts are preserved in `predecessor-F6F64E23/`; the intermediate
111533F3 build, flash and retry run are in `intermediate-111533F3/`.

## Final-image physical checks

| Check | Observed result | Retained transcript |
|---|---|---|
| Address and beacon boot controls | PASS, including failing old-constructor control | `retry-positive.log` |
| Two cancelled attempts with AP traffic | One client; both settled snapshots 11,176 free / 7,668 largest; two PRESENCE objects recovered; zero dropped/unscanned samples | `retry-positive.log` |
| Stop while connection worker is waiting | STOPPING then STOPPED only after cancellation; BLE teardown and Wi-Fi restoration; no panic or stale ready event | `stop-during-activation.log` |
| Windows central to board peripheral | Exact UUID/token, connection, service/characteristics, one exact 40-byte echo in three minimum-MTU fragments | `peripheral-host.log`, `peripheral-board.log` |
| Installed application verification | Complete flash readback SHA-256 equals the image above | `readback-verdict.txt`, `readback.log` |
| Post-verification control plane | Board idle; microphone/speaker available; BLE down; HTTP reachable over WiFi2 | `final-board-status.log`, `final-wifi-status.json` |

Scenario 6 uses a fixed diagnostic address and token and cancels each attempt
after five seconds. Its 512-byte settled-heap tolerance was specified before
the first run; both final snapshots were equal. The host played the canonical
`mcl-ap/conformance/vectors/01-presence-10b.wav` once during each wait. These
are configured diagnostics, and the node correctly reports `zero_prior=false`.
They do not replace a real refusal by Android or sustained failure-load tests.

Earlier-image readback attempts 1 and 2 failed in the serial transfer. Attempts 3 and 4 did
not finish and were stopped before retrying; no readback hash was accepted from
them. After resetting the application and using the native USB reset method,
the complete read succeeded and matched. The failed/incomplete transcripts are
retained; this does not establish the cause of the serial-transfer failures.

## Explicit negative host-peripheral attempt

Windows GATT service-provider tests returned
`STARTED_WITHOUT_ALL_ADVERTISEMENT_DATA`. The board observed the UUID but no
exact token match in two 40-second runs. Both expired with FAILED, zero
connections and zero carriage. These tests cannot qualify board-central
connection, and are retained as `windows-central-unavailable-*` logs. Scenario
7 is now available for the real Android advertiser; scenario 5 remains scan
only. No advertisement contract was relaxed to make Windows pass.

## Remaining release work

Android must first pass the independent Windows exact-token/GATT probe. Then
the corrected board must exercise real central carriage and the complete
zero-prior facade lifecycle, reverse-role regression and 3+ physical contention.
Only then may the release bundle be rebuilt and final gates retaken. Public
visibility, tagging and charter-required public review are separate gates.

Logs are retained as UTF-8 with LF line endings; PowerShell transcripts were
decoded from their original encoding without deleting lines. They contain lab
device addresses, local paths and private test topology. They remain private
evidence; publication requires the existing disclosure decision. No binary
image or factory backup is embedded in this directory. SHA256SUMS.txt covers
the retained text artifacts.
