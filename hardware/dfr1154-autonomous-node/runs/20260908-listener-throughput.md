# Continuous listening on the DFR1154: what it costs and what was fixed

**2026-09-08. Instrument measurement plus one over-air result.**

Most of this document is timing on the receiver, which is instrumentation and
carries no E-class. The last section records the first frames this node has
recovered over air, laptop to board. That run is **not** claimed as E-class
evidence either: it is a single unblinded run against a host transmitter, not a
counted campaign, and its purpose was to show the receive path works at all.

## Why it was measured

The autonomous node reported `heard=0` on every acoustic listen run while the
laptop transmitted PRESENCE at 6000/9000 Hz from about half a metre away. The
same emission was acquired by the laptop's own microphone at correlation 0.564,
so the transmitter was working and the band was reaching the room.

`heard=0` is not a diagnosis. A dead microphone, a transmitter pointed
elsewhere, and a receiver configured for a band nobody is using all produce it,
and this project has already lost a campaign to the third. So the node was
given a capture level meter before anything was concluded.

## What the meter said

    run end cap_blocks=55 cap_samples=56320 peak=18645 mean=1129

Peak 18 645 of 32 767 is −4.9 dBFS. The microphone was working and the room was
loud. But 56 320 samples in a 20 s run is **1.17 seconds of audio**: at 48 kHz
the run should have seen 960 000. The node was reading under 6% of the sound in
the room, so a 0.8 s frame almost always landed in a gap.

## Where the time went

    run end read_ms=214/25 poll_ms=20076/438 polls=55

Reading the microphone cost 214 ms of the 20 s. `mcl_ap_listen_poll` cost
20 076 ms — the entire run. The listener's own accounting was checked at the
same time and was **correct**:

    run end pushed=56320 searched=46720 unscanned=0 ref_len=9600

`searched == pushed − ref_len` exactly, so every sample was searched once, as
`ap_listen.h` specifies. Nothing was being re-scanned. The cost was real work.

## The comparison that located the defect

Experiment 008 decoded on this same board, and its run log records:

    captured=72000 ms=1473      (1.5 s of audio, in real time)
    LISTEN rc=0 ms=6027         (62 400 searched positions)

That is **0.097 ms per searched position**. The autonomous node was spending
**0.43 ms** — 4.5× more, on the same chip, the same modem and the same band.
The only structural difference is that 008 enters `mcl_ap_modem_decode` once
per capture and the continuous listener enters it on every poll.

So the suspect was per-call fixed cost, and there were two:

1. `generate_preamble_iq` rebuilt the 9 600-sample quadrature reference on
   every decode — a double-precision divide, a `sinf` and a `cosf` per sample,
   on a part whose FPU is single-precision only, so every double operation is a
   called routine.
2. `reference_stats` then recomputed that reference's mean and energy on every
   acquisition, twice — once at decimation stride and once in full — in two
   more double-precision passes.

Both are pure functions of three configuration fields. Neither depends on the
audio. A one-shot decoder pays them once per capture and never notices; a
listener polling many times a second pays them for nothing.

## The fix

Both results are cached in the caller-owned scratch, keyed on the config fields
that determine them and guarded by a magic word against an uninitialised
scratch. The reference produced is identical — same arithmetic, same order,
same values — so no decoded result moves. The loop-invariant chirp rate was
also hoisted out of the per-sample path.

## The effect, same board, same 20 s run

| | samples/s | ms per searched position |
|---|---|---|
| before | 2 816 | 0.430 |
| + reference cache | 7 731 | 0.134 |
| + statistics cache | **11 162** | **0.0888** |

Four times faster, and the per-position cost is now slightly better than
Experiment 008's single-shot figure — as it should be, since 008's number still
includes its own one-time reference build amortised over 62 400 positions.
Implementation overhead is gone; what remains is the correlation itself.

## What this does not fix

Real time at 48 kHz needs 48 000 searched positions per second. The board does
11 162. **It is still four times short, and no further implementation work
closes that**: the coarse acquisition pass is 3 200 complex multiply-accumulates
per sample of audio, about 154 million per second, against a single-issue
240 MHz single-precision FPU. Continuous acoustic listening is beyond this part
by arithmetic.

The consequence for the reference deployment is a design choice, not a bug: an
embedded receiver captures a bounded window and then decodes it — Experiment
008's shape, which reached E4 — and is deaf while decoding. AP-BOOTSTRAP-1's
repetition is what makes a duty-cycled receiver reachable. This is now stated
in `ap_listen.h` so a builder meets the number before choosing a design.

## The shape of what is missed, and the frame that arrived

Throughput was not the whole defect. At 380 ms per poll and 21 ms per read, the
node returned to the microphone only after each search, so the audio it did
capture arrived as 21 ms fragments separated by 380 ms holes. A frame is 0.8 s
of continuous signal; it cannot survive that pattern, and no amount of extra
throughput would have changed it.

Filling most of the window before polling keeps the same duty cycle and makes
the captured audio contiguous. Same board, same emission, 45 s, 20 PRESENCE
bursts from the laptop at 6000/9000:

    CONTACT 10 bytes at sample 172986
      object kind=0 source_ref=168947136 consumed=10
    CONTACT 10 bytes at sample 202441
    CONTACT 10 bytes at sample 248512
    CONTACT 10 bytes at sample 339872
    run end state=DONE recovered=4 objects=4
    run end cap_samples=451584 polls=12

`source_ref=168947136` is 0x0A11EDC0, which is what the transmitted payload
carries — the object was decoded, not merely acquired. Before this change every
run in this campaign reported `recovered=0`.

451 584 samples of a 45 s run is a **21% duty cycle**, and 4 recoveries from 20
emissions is **20%**. The recovery rate is the duty cycle, which is what a
deaf-while-decoding receiver predicts and is the reason AP-BOOTSTRAP-1 repeats.

## Verification

- `mcl-ap` test suite: 7/7 pass, including `mcl_ap_listen_test` and the
  Experiment 001 source decode.
- Release rehearsal: 17 gates pass, including the AP-BOOTSTRAP-1 vectors
  against both receivers and C4/C5 cross-implementation — which is what
  establishes that the caching changed no decoded byte.
- The one failing gate, release bundle reconstruction, is the deferred
  end-of-campaign rebuild and is unrelated.
