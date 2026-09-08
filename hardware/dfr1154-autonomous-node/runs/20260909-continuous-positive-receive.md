# Continuous positive receive on DFR1154

**Date:** 2026-09-09
**Scope:** implementation qualification, not AP profile promotion or independent interoperability evidence.

This receipt supersedes the feasibility conclusion in
`20260908-listener-throughput.md`. That earlier negative result remains retained:
the direct read-then-search loop was computationally deaf. The corrected node
separates microphone capture from decoding and uses a staged acquisition path.

## Exact implementation

- portable AP modem source SHA-256 prefix: `66D8F827A41293B9`
- flashed application SHA-256: `AB32FAEDC1F611CB87094D869BED40903BA29DCA837FEA9C2E848A56DDE2323A`
- application bytes: `1,268,640`
- target: ESP32-S3, 240 MHz, OPI PSRAM enabled
- hot listener window and modem scratch: internal DRAM
- capture queue: 131,072 PCM16 samples in PSRAM, single producer/single consumer
- acquisition: tap stride 18, offset step 9, weak-candidate gate 0.25 at the default 0.40 final threshold, three-point full-rate verification

## Positive traffic

The laptop played the canonical `01-presence-10b.wav` three times, three seconds
apart, while COM3 ran the autonomous listener for 22 seconds. The preceding
firmware image (before the final queue-drain-only change) reported:

```text
CONTACT 10 bytes    (three times)
source_ref=168947136 (0x0A11EDC0, exact transmitted value)
recovered=3
cap_samples=1058816
pushed=1056768
queue=2048/0
unscanned=0
capture dropped=0
read_ms=21574 max=25
poll_ms=6762 max=585
objects=3
```

The producer continued capturing while a positive decode occupied the consumer
for as long as 585 ms. All three objects were recovered byte-for-byte and no
audio was dropped or overwritten. The 2,048 queued samples were captured before
the run boundary but had not been drained when that image sealed its counters;
the final image drains the queue before sealing.

## Final-image quiet control

The final image above ran listen-only for eight seconds at 3000/6000 Hz:

```text
run end state=DONE heard=0 recovered=0 emitted=0 unscanned=0
run end cap_blocks=376 cap_samples=385024 peak=2234 mean=1118
run end pushed=385024 searched=375424 unscanned=0 queue=0/0
run end read_ms=7623/24 poll_ms=1823/20 polls=94
run end objects=0
```

This verifies that the final drain closes the accounting boundary, and that
polling no longer busy-spins while a pending candidate has no new samples.

## Host regression and retained corpus

- MSVC `/W4 /WX`: 7/7 AP test targets pass.
- 74 retained WAV files compared with the pre-optimization verdict ledger.
- 71 verdicts are identical.
- Three historical Android captures (`frame-trial-02`, `03`, `07`) improve from
  CRC failure to the same valid 24-byte object.
- No refusal/negative vector became accepted.

The threshold and strides above are evidence-bounded reference implementation
parameters. They are not normative acoustic constants. This receipt proves the
reference implementation can remain available through positive traffic on this
board; it does not replace the still-required DFR/Android independent physical
campaign, multi-machine contention, or public adoption rehearsal.
