# Central carriage, lifecycle and failed contention campaign

**Release is not approved.** This private laboratory record separates the
successful two-device runs from the failed three-device attempts. No Wire,
Link, AP waveform/profile, BLE role, token or zero-prior rule was changed.

## Discriminated failures and controls

1. `windows-probe-01.log` and `windows-probe-att.log`: independent Windows
   central to Android peripheral, exact diagnostic UUID/token, GATT discovery,
   and exact 40-byte / three-fragment return. Configured diagnostic, not
   zero-prior contact.
2. `central-*`, `matrix-*`, `allocation-failure.log`: DFR connected and
   subscribed but its data writes did not reach Android. Moving writes between
   tasks, requesting responses and using the native no-response API did not
   repair this. The allocation hook reported a 27-byte internal DMA allocation
   failure (`caps=00000808`) despite available general heap. This establishes
   the later data-transmission defect; it does not retroactively identify the
   cause of the earlier first-connection failure.
3. `dma-staging-central.log`: moving the cold 2048-byte capture-consumer buffer
   to PSRAM, while retaining the decoder window/scratch internally, restored
   the original three-fragment exact echo. DMA capacity after activation was
   1588 bytes with a 1332-byte largest block. `intentional-wrong-echo.log`
   deliberately injected `DEADBEEF`; its failure is a negative control.
4. `lifecycle-01-*`: advertising alone was incorrectly reported as candidate
   readiness. Both adapters now wait for notification subscription.
5. `lifecycle-02-*`: a repeated OFFER tore down the pending candidate while
   replaying ACCEPT. The coordinator now retains that candidate and replays
   the same acceptance. BLE teardown no longer permanently releases the
   controller memory needed for a later activation in the same boot.
6. `lifecycle-04-*`, `uncertain-before.log`, `uncertain-after.log`: queued BLE
   transmission was treated as definite refusal when initiating validation.
   Queue admission now preserves the candidate and still requires the real
   matching response. Negative controls with undelivered controls establish
   no contact and release candidates after bounded failure.
7. `blocking-clock-before.log` / `blocking-clock-after.log`: a 1500-ms blocking
   transmit consumed part of the peer's frozen 6000-ms response window. The
   existing window now starts after emission returns. Its duration did not
   change. All nine SDK test targets passed after the correction.
8. `final-lifecycle-02-*`: Android received a handoff notification before its
   next readiness-service tick, advanced to policy, then rejected late
   candidate readiness. The adapter now queues up to eight received frames
   and publishes readiness before delivering them to the facade. The final
   APK is `02C0EA5B0E30502DE1C5BFF25BA03330877AE0CDD3C0991C9E31BE2B0BDFA7FC`.

## Physical results

| Run | Result and scope |
|---|---|
| lifecycle-03 | Both devices migrated; board peripheral; session `88BE376B` |
| lifecycle-05 | Both devices migrated; board peripheral; session `E1C66C53` |
| lifecycle-06 | No migration; connection status 62 and later acoustic retry failure retained |
| lifecycle-07 | Both devices migrated; board central; session `67545F2A`; Android records actual ATT writes |
| contention-01 | Three physical facades emitted/recovered AP objects; no migration; not qualified |
| contention-02 | Candidate negotiation reached Windows, which could not advertise the required complete beacon; no migration; not qualified |
| final-lifecycle-02 | Readiness-order race reproduced; no migration |
| final-lifecycle-03 | Current image/APK; both migrated with board central; session `E54DB407` |
| final-lifecycle-04 | Current image/APK; both migrated with board central; session `DB6E5978` |
| final-lifecycle-05 | Current image/APK; both migrated with board peripheral; session `4FF8E555`; notification-before-service ordering handled |

The successful lifecycle logs report independently generated local references,
peer values learned over air/radio, explicit policy admission, and matching
session references at both endpoints. ADB/serial carried commands and logs,
not MCL protocol frames. The configured fixed-token diagnostics remain
`zero_prior=false`.

The Windows port is experimental. Windows reported
`StartedWithoutAllAdvertisementData`; a separate raw publisher emitted a token
but did not complete the connection. A scan is not carriage. These attempts
do not provide the third conformant machine required to close the physical
contention criterion.

## Current image verification

Application SHA-256:
`0DF1D23A7F710840EB1DD5A85D122C4D1A9DA2E89A15D24E57A540C296E5702C`
(1,275,952 bytes). `final-readback-sha256.log` records an exact full application
partition readback match. Only the application at `0x20000` was written.

`final-retry-audio.log`: two cancelled attempts reuse one client, both settle
at heap 13152 / largest 7668 and DMA 5392 / largest 5364; one acoustic PRESENCE
is decoded during each attempt; no capture drops. `final-stop.log`: STOP during
activation cancels successfully, tears down BLE and restores the control plane.

The current image contains the later blocking-send timing correction. Final
runs 03/04 and 05 establish both orientations on this image and the final APK;
the earlier successes remain evidence for their recorded predecessor images.

## Evidence handling

`logs/` retains successful and failed tool/device output, normalized only from
UTF-16/UTF-8 BOM and CRLF to UTF-8/LF. Complete phone bugreports and unrelated
Bluetooth data remain local; `private-trace-reference.txt` identifies the raw
HCI capture by hash. The retained HCI parser is diagnostic tooling, not a
replacement for the physical endpoint logs.

Build manifests identify the board and APK. `source-sha256.txt` identifies the
current canonical and adapter sources; predecessor build logs retain their
own hashes. `SHA256SUMS.txt` seals the retained record. The release bundle and
public review/disclosure decisions are separate gates.
