# Android to DFR1154 zero-prior attempt 01

Date: 2026-09-09 (Asia/Calcutta)

Verdict: **FAILED, retained as implementation evidence.** The two independent
platforms exchanged enough bootstrap traffic to prove both acoustic directions,
but one lost `TRANSPORT_ACCEPT` could not recover because the DFR adapter stopped
decoding AP traffic as soon as BLE activation began.

## Provenance and topology

- Android device: `13793355820008D`, model `I2017`, connected over USB/ADB.
- Installed APK SHA-256:
  `82197B027FFB5FEFBC9AC13B9F1118E87EC0052B17028D96EB831788901D1570`.
- DFR1154: no USB/COM connection during the run; controlled only through its
  isolated Wi-Fi 2 adapter at `192.168.4.1`.
- DFR application image SHA-256:
  `7156786FC454C2FD275E3F26FCBA3CD693C7D2225A788441455386E75247B36F`.
- No peer address, BLE address, endpoint token, source reference, session
  reference, pairing, or shared secret was configured on either endpoint.
- Board scenario 2: 120 seconds, frozen default AP band, gain 100%, Wi-Fi
  quiesced, BLE candidate transport 3/profile 1, local policy admit.

The Android APK was rebuilt and reinstalled after this attempt from the same
machine/protocol sources. That post-attempt artifact is
`A66BCAF88F4DAC0CBF3A9B1433B32E3EA7FE289549C4F68AAF39D80E779FCA8C`;
the intervening repository changes before the rebuild were packaging and
receipts, not protocol or Android machine logic.

## Observed exchange

1. Android decoded the DFR `PRESENCE` and reported peer reference `2C36B204`.
2. Android emitted repeated 17-byte `TRANSPORT_OFFER` objects.
3. DFR decoded one offer, learned endpoint token `640C704B` **from air**, and
   emitted one 16-byte `TRANSPORT_ACCEPT`.
4. Android did not recover that acceptance, so it did not advertise the BLE
   candidate.
5. DFR scanned for the air-learned token and saw no match; Android eventually
   reported `NO_COMMON_BEARER`.

This is not evidence of a broken speaker or microphone: each platform decoded a
frame emitted by the other. It is also not acceptable as ordinary channel loss.
The coordinator already retransmits the same `OFFER` and idempotently re-arms the
same acceptance when it hears that retry. The DFR adapter made this recovery
unreachable by polling AP only in `PHASE_RENDEZVOUS`; immediately after its first
acceptance it changed to `PHASE_ACTIVATE`, while its capture task continued to
collect and later discard the repeated offers.

## Sealed DFR result

```json
{"run_state":"DONE","phase":"REPORT","scenario":2,"zero_prior":true,"log_dropped":0,"ble_frames_lost":0,"ble_role":"CENTRAL_ACCEPTOR","machine_state":"IDLE","failure":"","counters":{"frames_heard":0,"frames_recovered":1,"frames_emitted":11,"objects_decoded":0,"samples_unscanned":1032960,"ble_frames_tx":0,"ble_frames_rx":0,"ble_frag_rejected":0,"scan_matches":0,"scan_uuid_only":0,"cap_blocks":4787,"cap_samples":4901888,"cap_peak":3111,"cap_mean":1108,"scan_slices":5,"scan_seen":6},"values":[{"name":"own_source_ref","value":741782020,"provenance":"LOCAL"},{"name":"peer_endpoint_token","value":1678536779,"provenance":"FROM_AIR"}]}
```

## Corrective build

The adapter now continues AP decoding through `PHASE_ACTIVATE` and stops only
after the candidate has carried traffic into validation/policy. The coordinator
and its existing deterministic lost-acceptance mutation remain unchanged.

The corrected, compiled but not yet flashed DFR application image is:

```text
SHA-256  1B60FBD008194EB3182A2F94859682D9793F0E2E9F960515A9A577CBDDD2A33B
bytes     1,268,736
flash     1,268,583 / 3,145,728
DRAM      241,392 / 327,680
headroom  86,288
```

No COM port was used to build it. The next physical attempt requires an
application-partition-only flash after the single USB-C connection is moved from
Android to DFR1154.
