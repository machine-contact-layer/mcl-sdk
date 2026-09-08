package org.mcl.bench;

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioRecord;
import android.media.AudioTrack;
import android.media.MediaRecorder;

/**
 * The phone's speaker and microphone, at exactly 48 kHz, mono, PCM 16.
 *
 * <p><b>Exactly</b> is the whole point. Android will happily resample: ask for
 * 44 100 and the platform gives you 44 100 while the hardware runs at 48 000,
 * and the difference lands in the timing estimator as drift that looks like a
 * bad channel. {@code mcl-ap} has been caught by rate assumptions before, so
 * the configured rate is checked against what the objects actually report and
 * a mismatch is a refusal, not a warning.
 *
 * <p>The other Android-specific trap is the DSP. {@link MediaRecorder.AudioSource#VOICE_RECOGNITION}
 * is used rather than {@code MIC} because it is the source least likely to have
 * automatic gain control, noise suppression and echo cancellation applied — all
 * of which are designed to destroy exactly the kind of narrowband tone this
 * protocol carries. It is not a guarantee: the vendor decides, and that is why
 * the phone's path is measured rather than assumed.
 */
public final class AudioBench {

    public static final int SAMPLE_RATE = 48000;

    private AudioTrack track;
    private AudioRecord record;
    private final Logger log;

    public interface Logger {
        void line(String text);
    }

    public AudioBench(Logger log) {
        this.log = log;
    }

    /* ------------------------------------------------------------ output */

    /**
     * Play PCM synchronously and return when the samples have been handed to
     * the hardware. Blocking on purpose: a transmit that returned before the
     * sound left would make every timing measurement about the buffer.
     */
    public boolean play(short[] samples, int count, float gain) {
        final int minBuffer = AudioTrack.getMinBufferSize(
                SAMPLE_RATE, AudioFormat.CHANNEL_OUT_MONO,
                AudioFormat.ENCODING_PCM_16BIT);
        if (minBuffer <= 0) {
            log.line("AUDIO play refused: no output buffer size for 48 kHz mono");
            return false;
        }
        final int bufferBytes = Math.max(minBuffer, count * 2);

        track = new AudioTrack.Builder()
                .setAudioAttributes(new AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                        .build())
                .setAudioFormat(new AudioFormat.Builder()
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .setSampleRate(SAMPLE_RATE)
                        .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                        .build())
                .setBufferSizeInBytes(bufferBytes)
                .setTransferMode(AudioTrack.MODE_STREAM)
                .build();

        if (track.getSampleRate() != SAMPLE_RATE) {
            log.line("AUDIO play REFUSED: track runs at " + track.getSampleRate()
                     + " Hz, not " + SAMPLE_RATE);
            track.release();
            track = null;
            return false;
        }

        short[] scaled = samples;
        if (gain < 1.0f) {
            scaled = new short[count];
            for (int i = 0; i < count; i++) {
                scaled[i] = (short) (samples[i] * gain);
            }
        }

        int peak = 0;
        long squareSum = 0L;
        for (int i = 0; i < count; i++) {
            final int value = scaled[i];
            final int magnitude = (value == Short.MIN_VALUE)
                    ? 32768 : Math.abs(value);
            peak = Math.max(peak, magnitude);
            squareSum += (long) value * (long) value;
        }
        final int rms = (count == 0)
                ? 0 : (int) Math.sqrt(squareSum / (double) count);

        /* Per-track maximum is independent of the user's MEDIA stream level;
           log enough physical-output facts that a silent observation can be
           distinguished from an empty buffer or a track that never started. */
        track.setVolume(AudioTrack.getMaxVolume());
        log.line("AUDIO play START requested=" + count
                 + " duration_ms=" + (count * 1000L / SAMPLE_RATE)
                 + " peak=" + peak + " rms=" + rms + " gain=" + gain
                 + " state=" + track.getState()
                 + " buffer_frames=" + track.getBufferSizeInFrames());

        track.play();
        log.line("AUDIO track PLAYING play_state=" + track.getPlayState()
                 + " session=" + track.getAudioSessionId());
        int written = 0;
        while (written < count) {
            final int n = track.write(scaled, written, count - written,
                                      AudioTrack.WRITE_BLOCKING);
            if (n <= 0) {
                log.line("AUDIO play stalled at " + written + " of " + count);
                break;
            }
            written += n;
        }
        /*
         * WAIT FOR THE SPEAKER, NOT FOR THE BUFFER. This emitted silence for
         * an entire campaign.
         *
         * The buffer above is sized to hold the whole waveform, so
         * WRITE_BLOCKING returns as soon as the samples are QUEUED -- about
         * 270 ms for 1.2 s of audio, most of that spent building the array.
         * A fixed 120 ms sleep after it therefore tore the track down with
         * roughly nine tenths of the frame still unplayed. Worse, an
         * AP-BOOTSTRAP-1 waveform opens with 0.1 s of leading silence and a
         * 0.2 s preamble, so what actually reached the speaker was only the
         * beginning of the envelope and none of its payload: the phone
         * logged "played 57600 samples" while emitting no sound at all, and a
         * laptop microphone half a metre away recorded no 6 kHz or 9 kHz
         * energy whatsoever.
         *
         * getPlaybackHeadPosition() counts frames the hardware has actually
         * consumed, so it is the only honest way to ask whether the sound has
         * left. Poll it to the end of the waveform, then add a short settle
         * for the tail still in flight.
         */
        final long deadline = System.nanoTime()
                + (long) (count / (double) SAMPLE_RATE * 1e9) + 2_000_000_000L;
        while (track.getPlaybackHeadPosition() < written
               && System.nanoTime() < deadline) {
            try {
                Thread.sleep(10);
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
                break;
            }
        }
        final int played = track.getPlaybackHeadPosition();
        if (played < written) {
            log.line("AUDIO play INCOMPLETE: " + played + " of " + written
                     + " frames left the hardware");
        }
        /* The head position reaching the end means the last frame has been
           consumed by the mixer, not that it has finished sounding. */
        try {
            Thread.sleep(80);
        } catch (InterruptedException ignored) {
            Thread.currentThread().interrupt();
        }
        final int underruns = track.getUnderrunCount();
        track.stop();
        track.release();
        track = null;
        log.line("AUDIO played " + written + " samples head=" + played
                 + " underruns=" + underruns
                 + " gain=" + gain);
        return written == count;
    }

    /* ------------------------------------------------------------- input */

    public boolean openMicrophone() {
        final int minBuffer = AudioRecord.getMinBufferSize(
                SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT);
        if (minBuffer <= 0) {
            log.line("AUDIO capture refused: no input buffer size for 48 kHz mono");
            return false;
        }
        try {
            record = new AudioRecord(MediaRecorder.AudioSource.VOICE_RECOGNITION,
                                     SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO,
                                     AudioFormat.ENCODING_PCM_16BIT,
                                     minBuffer * 4);
        } catch (SecurityException denied) {
            log.line("AUDIO capture refused: RECORD_AUDIO not granted");
            return false;
        }
        if (record.getState() != AudioRecord.STATE_INITIALIZED) {
            log.line("AUDIO capture refused: recorder would not initialise");
            record.release();
            record = null;
            return false;
        }
        if (record.getSampleRate() != SAMPLE_RATE) {
            log.line("AUDIO capture REFUSED: recorder runs at "
                     + record.getSampleRate() + " Hz, not " + SAMPLE_RATE);
            record.release();
            record = null;
            return false;
        }
        record.startRecording();
        log.line("AUDIO capture open, source=VOICE_RECOGNITION rate="
                 + record.getSampleRate());
        return true;
    }

    public int read(short[] into) {
        if (record == null) {
            return -1;
        }
        return record.read(into, 0, into.length);
    }

    public void closeMicrophone() {
        if (record == null) {
            return;
        }
        record.stop();
        record.release();
        record = null;
        log.line("AUDIO capture closed");
    }

    public boolean isCapturing() {
        return record != null;
    }
}
