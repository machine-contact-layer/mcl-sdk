# Bounded physical contention closure: HOLD

The owner clarified that the third node is an independent physical AP
participant, not a required third complete BLE port. See V1_SCOPE section 5.10.
No wire, election, token, deployment or peer-selection rules were changed.
The earlier sealed audit is retained as historical evidence; its demand for
a third complete BLE peer is superseded by this clarification.

All trials use DFR1154 on COM3, Android through wireless ADB, and the laptop's
own WASAPI microphone/speaker (18/14). ADB/serial carry laboratory start/stop,
logging and explicit local policy admission, never a peer locator. Windows
uses OS randomness and the canonical machine facade. No peer is filtered to
force DFR/Android selection. The damaged laptop speaker is not claimed as a
transmitter-diversity result.

The installed Android APK was rechecked as
02c0ea5b0e30502de1c5bff25ba03330877ae0cdd3c0991c9e31be2b0bdfa7fc.
The Windows bridge rebuilt without changing digest
9e94ef1a60316e816edbd60c35dfd009b6b71e062fd24c9a79232333e568bf7b,
and its canonical PCM and fragmentation checks passed.

Trial names describe intended stresses, not passing verdicts.
`closure-cell1` was invalid: Android's Activity was absent. A successful ADB
broadcast did not mean its dynamic receiver existed. Subsequent trials check
for the process and start the Activity beforehand.

`closure-cell1-valid`: all three exchanged physical AP traffic, but Windows
was selected and its candidate failed. DFR ended EXHAUSTED; no DFR/Android
migration. This is a retained negative result, not proof of cyclic agreement.

`closure-cell2`: simultaneous start with Windows role 1; Android and DFR
migrated with matching session E8D0B060. Windows emitted repeated PRESENCE.
Collision-recovery qualification must not be inferred merely from overlapping
emissions or missing decoder output. Detailed final verdict follows below.


`closure-cell3`: Windows startup delayed by 3 seconds. DFR/Android selected
each other, but the board processed policy before its connection worker
published readiness and closed the candidate. This exposed a board adapter
ordering defect, not evidence that Windows needs a complete BLE port.
The board fix retains the existing bounded receive mailbox until readiness
has been published and the worker is idle. No protocol change was made.
New application digest:
C0D69F3EB615553E4050871CA7FB95C81527835D8E83A7A87386D106E319D428.

The subsequent fixed-image campaign adds raw Windows microphone recording,
including local transmission, bounded to 300 seconds. Offline decoding is
labelled as such and does not replace live rendezvous participation evidence.


## Final traced results and limits

The instrumented board image is
`EEA8DA8D63078D29ECD836967D9C422CAA55BA98007CF9287F3D030C5EF881F4`;
the instrumented APK is
`E997AEA073AEA516F9C79B0CBAC23E171638D46887A49DBAE1779B7B4203B0B8`.
They supersede the intermediate image above. The source and build manifests,
raw device logs and microphone captures are retained in the sibling SDK at
`hardware/dfr1154-autonomous-node/runs/20260909-contention-closure/`.

| Traced run | DFR/Android session | What the evidence establishes |
| --- | --- | --- |
| cell1 | E0585224 | All three transmitted and decoded AP; both devices received Windows traffic. DFR selected Android's ACCEPT before a later Windows ACCEPT of the same offer, retained the first session and migrated. |
| cell2 | 46269E56 | Deliberate overlapping startup emissions; retry followed by migration. Board explicitly deferred an early 34-byte BLE frame until worker readiness, exercising the fix. Windows had no live valid AP decode in this cell. |
| cell3 | 08810099 | Delayed third-party transmissions and migration; Android decoded Windows PRESENCE after migration. The required pre-migration interference is not established by this run alone. |

The first cell supplies stronger pre-migration foreign-ACCEPT evidence than
the third cell. AP-BOOTSTRAP-1 section 8.1 permits competing proposals and
binds the offerer to its first matching ACCEPT; a losing proposal is not itself
a second established pair. The logs show no three-edge ACCEPT cycle and only
one established pair. The verifier checks these observations, not universal
absence of future failures.

**The requested three-cell campaign is not yet fully qualified.** In
particular, offline recovery from the Windows recording is not evidence that
the live Windows state machine consumed those frames. Likewise a microphone
HEARD result during overlapping playback is not, alone, causal isolation of
collision from speaker distortion. These limits are not waived to close the
release ledger. The already demonstrated two-device and three-device migration
results remain valid with their stated bounds.

Final board readback matches the built image exactly. Two cancelled attempts
have equal settled heap/DMA figures, one reused client and zero capture drops;
each decoded a real Android PRESENCE. STOP cancelled the worker and restored
the control plane. MSVC ran 43 targets successfully on these source changes.
The release bundle remains held pending physical campaign closure.


## Artifact provenance

All files under evidence/ were generated locally by this laboratory campaign:
board application built from the canonical sources with the qualified Arduino
ESP32 core, APK built from the same canonical sources through the NDK, and
mono 48 kHz signed 16-bit WAV recordings from the Windows physical microphone.
Recordings include room sound and local transmissions. They remain private
research evidence, not a public release asset or synthetic conformance vector.
No factory firmware, unrelated Android bugreport or third-party binary is
included. SHA256SUMS.txt seals all files except itself; source-sha256.txt binds
the source inputs. Text logs are normalized to UTF-8/LF without removing lines.

Run verify_closure.py with this directory's logs/ path to reproduce the three
traced-session checks. A successful verifier exit does not close the missing
physical predicates described above; its release_approved field is false.
The orchestration scripts preserve the original local laboratory paths and
are retained for audit, not presented as portable SDK APIs.


The bounded collision recheck `closure-trace-cell2-recheck` on 2026-09-10
recorded Windows live AP recovery and carrier_busy=1 followed by carrier_busy=0
before transmission. DFR recovered 17 frames and heard 3 unrecovered frames,
but no pair migrated within 120 seconds. Windows ended EXHAUSTED and DFR ended
ANNOUNCING. This is a negative cell, not a pass assembled from another trial.
All participants were stopped or had returned to their reporting state.
