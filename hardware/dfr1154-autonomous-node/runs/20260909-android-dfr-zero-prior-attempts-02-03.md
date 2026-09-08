# Android to DFR1154 zero-prior attempts 02 and 03

Date: 2026-09-09 (Asia/Calcutta)

Verdict: **FAILED, retained as implementation evidence.** The correction from
attempt 01 worked: Android received the DFR `TRANSPORT_ACCEPT`, advertised the
air-learned endpoint token, and the DFR matched that exact token. The full
facade then failed at the next boundary: the DFR central could not establish a
BLE connection. A clean Android Bluetooth disable/enable between attempts did
not change the result.

## Provenance and frozen configuration

- Android device: `13793355820008D`, model `I2017`, Android 14, connected over
  USB/ADB.
- Installed APK SHA-256:
  `DAFE07DC8C60CC9A6E31EE0D9A461B2B8B1A286E96D1B25383DF5BF3A4DFA683`
  (70,109 bytes), rebuilt from the current canonical sources before the runs.
- DFR1154: no USB/COM connection during either run; controlled only through
  its isolated Wi-Fi 2 adapter at `192.168.4.1` between runs.
- DFR application image SHA-256:
  `1B60FBD008194EB3182A2F94859682D9793F0E2E9F960515A9A577CBDDD2A33B`.
  Before USB was moved to Android, the application partition was read back and
  matched this image byte-for-byte. Bootloader and partition table were not
  reflashed.
- SDK source commit in the image: `7ef1f18`.
- Scenario 2, 120,000 ms, default AP band, gain 100%, Wi-Fi quiesced, BLE
  candidate transport 3/profile 1, local policy admit.
- No peer address, BLE address, endpoint token, source reference, session
  reference, pairing, or shared secret was configured on either endpoint.

## Boundary reached twice

Both attempts followed the same path:

1. Android decoded the DFR `PRESENCE`.
2. Android emitted a 17-byte `TRANSPORT_OFFER`.
3. DFR decoded the offer, learned its endpoint token **from air**, selected the
   central/acceptor role, and emitted a 16-byte `TRANSPORT_ACCEPT`.
4. Android received the acceptance, opened its GATT server and advertised the
   selected token.
5. DFR saw only exact UUID-and-token matches (`scan_uuid_only=0`) and learned
   the BLE address **from bearer**.
6. Each DFR `connect(address, random-address-type)` call blocked for about 30
   seconds and returned false. No candidate frame crossed BLE, so path
   validation, policy and COMMIT/CONFIRM were never reached.

Attempt 02 used endpoint token `AE4E607B`; the DFR recorded five exact matches
from nine advertisements and four refused connection attempts. Attempt 03,
after Android Bluetooth was disabled and re-enabled, used endpoint token
`AA74B811`; it again recorded five exact matches from nine advertisements and
four refused connection attempts. Android reported its GATT server and
advertiser started in both runs.

## Sealed DFR results

Attempt 02:

```json
{"run_state":"DONE","phase":"REPORT","scenario":2,"zero_prior":true,"log_dropped":0,"ble_frames_lost":0,"ble_role":"CENTRAL_ACCEPTOR","machine_state":"SOLICITING","failure":"","counters":{"frames_recovered":1,"frames_emitted":4,"ble_frames_tx":0,"ble_frames_rx":0,"scan_matches":5,"scan_uuid_only":0,"scan_seen":9},"values":[{"name":"peer_endpoint_token","provenance":"FROM_AIR"},{"name":"peer_ble_address","provenance":"FROM_BEARER"}]}
```

Attempt 03:

```json
{"run_state":"DONE","phase":"REPORT","scenario":2,"started_ms":3345,"ended_ms":137266,"zero_prior":true,"log_dropped":0,"ble_frames_lost":0,"ble_role":"CENTRAL_ACCEPTOR","machine_state":"ANNOUNCING","failure":"","counters":{"frames_heard":0,"frames_recovered":1,"frames_emitted":4,"objects_decoded":0,"samples_unscanned":5721856,"ble_frames_tx":0,"ble_frames_rx":0,"ble_frag_rejected":0,"scan_matches":5,"scan_uuid_only":0,"cap_blocks":5947,"cap_samples":6089728,"cap_peak":2264,"cap_mean":1103,"scan_slices":0,"scan_seen":9},"values":[{"name":"own_source_ref","value":2696460580,"provenance":"LOCAL"},{"name":"peer_endpoint_token","value":2859776017,"provenance":"FROM_AIR"},{"name":"peer_ble_address","value":480850540,"provenance":"FROM_BEARER"}]}
```

Attempt 03's four exact-match connection attempts began at 12,817, 44,761,
74,900 and 106,841 ms. They returned `BLE connect refused` at 42,819, 74,763,
104,902 and 136,843 ms respectively.

## Resource observation and claim boundary

The retained standalone DFR/Android BLE-ACTIVATE proof connected and carried
fragmented GATT frames in this same central orientation. Its board log recorded
`BLE up free_heap=16124 largest=8180`. The complete-facade attempts recorded:

```text
attempt 02  BLE up free_heap=5412 largest=3188
attempt 03  BLE up free_heap=5268 largest=3060
```

This is strong evidence that the failure is specific to the full adapter's
runtime resource envelope, not to the frozen BLE role rule or advertisement
format. It is **not yet causal proof** that heap exhaustion is the sole reason
for the connection refusal. No protocol parameter or role rule was changed to
make the run pass.

The installed board image cannot be changed now: the single USB-C connection
has moved to Android, and the DFR is reachable only through Wi-Fi 2. Therefore
these runs do not close the two-builder migration gate. Any firmware correction
requires a later application-partition flash and a new physical rerun; host
tests or a successful build alone cannot replace it.

## Subsequent audit correction (2026-09-09)

The resource paragraph above records the prior inference; it is not accepted
as a causal verdict. The standalone heap/carriage receipt was peripheral-role
evidence, not a demonstrated successful central connection with 16 KB free.
Repeated allocations also mean four retries were not four clean independent
repetitions. The corrected adapter now addresses the blocking operation, client
lifetime, scan continuation and a deterministic native-address reversal.
The original failure still needs a discriminating peer run after correction.
No zero-prior, reverse-role or contention success is inferred from these fixes.
