# MCL Android bench

A lab instrument. An Android phone as a second physical MCL machine, so that
protocol claims can be tested between two independent implementations on two
independent clocks rather than between a board and the laptop that programmed
it.

**Not a product, not an SDK sample, and not a conformance implementation.**
Nothing here is cited as evidence on its own; it is one end of a rig.

## What it is made of, and what it is not made of

The MCL side is the **canonical C**, compiled for the phone through the NDK and
reached over JNI. `build-apk.ps1` copies the protocol sources from their
repositories at build time and prints the SHA-256 of every one of them next to
the APK's, exactly as the firmware build does. There is no Kotlin or Java
implementation of any MCL behaviour, and there must not be: two hand-written
implementations that agree prove that two people read the same specification
the same way, which is not what this rig is for.

Java is used only for what Java is the only way to reach — the microphone, the
speaker, the Bluetooth adapter, and the activity that owns them.

## Control plane

adb, and adb only. Commands arrive as broadcasts and results go to logcat. The
app requests **no network permission at all**, so it cannot open a socket even
by accident, and no MCL byte can reach the peer except across the air or the
radio.

    .\bench.ps1 -Restart -Command 'band 3000 6000'
    .\bench.ps1 -Command 'emit PRESENCE 100' -Listen 4
    .\bench.ps1 -Command 'listen 30000'
    .\bench.ps1 -Command 'ble on' -Then 'adv A5C30F17'
    .\bench.ps1 -Tail 40

The quoting in `bench.ps1` is not decoration: `--es cmd "emit PRESENCE"`
delivers only `emit`, because the device-side shell splits the string a second
time. The extra has to survive two shells.

Two-machine drivers live beside it: `two-machine-ladder.ps1` runs one band rung
between the phone and the DFR1154 in either direction, and `three-objects.ps1`
runs the three AP-BOOTSTRAP-1 object sizes and reports them separately.

## Building

No Gradle, and nothing is downloaded. `build-apk.ps1` drives `javac`, `jar`,
`d8`, `aapt2`, `zipalign` and `apksigner` directly against an SDK and NDK
already on the machine, and the paths are parameters. Two things that are not
obvious and cost time to find:

- **build-tools 35, not 34.** 34.0.0 ships an R8 whose dexer dies on this app's
  anonymous `BroadcastReceiver` with an internal null-pointer error. 35.0.0
  dexes the identical jar without complaint.
- `JAVA_HOME` must be set, because `d8.bat` and `apksigner.bat` resolve java
  through it whatever is on `PATH`.

## Traps this rig has already been caught by

Each of these produced a confident, wrong reading before it was found. They are
listed because the next person will meet them too.

- **`BLUETOOTH_SCAN` without `neverForLocation` returns no scan results at
  all** on Android 12 and later unless `ACCESS_FINE_LOCATION` is also held. No
  error and no denial — the callback is simply never invoked, so a peer
  advertising steadily for fifty seconds is indistinguishable from no peer. The
  flag is claimed in the manifest and it is accurate: this scanner matches on
  advertisement contents and derives nothing about location.
- **`AudioTrack.write(..., WRITE_BLOCKING)` returns when the samples are
  queued, not when they are played.** With a buffer sized to hold the whole
  waveform it returns almost immediately, and tearing the track down after a
  fixed sleep emitted a fraction of every frame — silence, in practice, since
  an AP-BOOTSTRAP-1 waveform opens with leading silence and a preamble. The
  phone logged `played 57600 samples` throughout. `play()` now polls
  `getPlaybackHeadPosition()` to the end of the waveform, and logs peak, RMS,
  play state, head position and underruns so that a silent observation can be
  told apart from an empty buffer.
- **Do Not Disturb silently reverts adb volume changes**, and the media volume
  may be capped by the vendor ROM below its nominal maximum. `--set`,
  `--adjust` and `audio_safe_volume_state` may all be refused. Check the level
  that came back, not the one that was asked for.
- The capture source is `VOICE_RECOGNITION` rather than `MIC` because it is the
  source least likely to have AGC, noise suppression and echo cancellation
  applied — all of which are built to destroy narrowband tones. It is not a
  guarantee; the vendor decides.

## Evidence taken with it

- `mcl-ble/hardware/host-ble-probe/runs/20260908-dfr1154-android-both-orientations.md`
  — BLE-ACTIVATE-1 in both role orientations and BLE-GATT-1 carriage.
- `mcl-ap/experiments/012-multi-transmitter-band/evidence/20260908-dfr-android-frame-ladder.md`
  — the acoustic band ladder and the three object sizes, including what that
  campaign failed to establish.
