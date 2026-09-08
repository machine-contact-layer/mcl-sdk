package org.mcl.bench;

import android.os.Handler;

import java.util.Arrays;

/**
 * Android's bounded port of the product-facing {@code mcl_machine_t} API.
 * Protocol sequencing remains native; this class supplies only audio, BLE,
 * scheduling, and candidate-bearer lifecycle.
 */
public final class MachineAdapter implements AutoCloseable, Mcl.MachinePlatform {
    private static final int TRANSPORT_AP = 1;
    private static final int TRANSPORT_BLE = 3;
    private static final int EVENT_POLICY_REQUIRED = 2;
    private static final long SERVICE_MS = 10L;

    private final Handler work;
    private final AudioBench audio;
    private final BleBench ble;
    private final AudioBench.Logger log;
    private final int bandLow;
    private final int bandHigh;
    private final long handle;

    private volatile boolean running;
    private volatile boolean listening;
    private volatile boolean selfTransmitting;
    private volatile boolean mediumBusy;
    private long listener;
    private Thread listenerThread;
    private boolean candidateOpening;
    private boolean closed;

    public MachineAdapter(Handler work, AudioBench audio, BleBench ble,
                          AudioBench.Logger log, int sourceRef, int role,
                          int bandLow, int bandHigh) {
        this.work = work;
        this.audio = audio;
        this.ble = ble;
        this.log = log;
        this.bandLow = bandLow;
        this.bandHigh = bandHigh;
        handle = Mcl.machineCreate(sourceRef, role, this);
        if (handle == 0L) {
            throw new IllegalStateException("native machine initialization refused");
        }
    }

    public void start() {
        if (running) {
            return;
        }
        listener = Mcl.listenerCreate(17, bandLow, bandHigh, 96000);
        if (listener == 0L || !audio.openMicrophone()) {
            if (listener != 0L) {
                Mcl.listenerDestroy(listener);
                listener = 0L;
            }
            throw new IllegalStateException("continuous acoustic input refused");
        }
        running = true;
        listening = true;
        startListenerThread();
        final int status = Mcl.machineStart(handle);
        if (status != 0) {
            close();
            throw new IllegalStateException("machine start status=" + status);
        }
        log.line("MACHINE started source_ref is local; poll_ms=" + SERVICE_MS);
        work.post(service);
    }

    /** Called synchronously by native MCL; preserve its three-way TX result. */
    @Override
    public int transmit(int transportId, byte[] bytes) {
        if (transportId == TRANSPORT_AP) {
            final int samples = Mcl.modulatedSamples(bytes.length, bandLow, bandHigh);
            if (samples <= 0) {
                return -1;
            }
            final short[] pcm = new short[samples];
            final int used = Mcl.modulate(bytes, pcm, bandLow, bandHigh);
            if (used <= 0) {
                return -1;
            }
            selfTransmitting = true;
            Mcl.machineSetMediumState(handle, false, true);
            try {
                return audio.play(pcm, used, 1.0f) ? 0 : -1;
            } finally {
                selfTransmitting = false;
                Mcl.machineSetMediumState(handle, false, false);
            }
        }
        if (transportId == TRANSPORT_BLE) {
            /* Android only reports queue admission here, not on-air completion. */
            return ble.sendFrame(bytes) ? 1 : -1;
        }
        return -1;
    }

    public void receiveBle(byte[] frame) {
        if (running) {
            work.post(() -> receive(TRANSPORT_BLE, frame));
        }
    }

    public void policy(boolean admit) {
        work.post(() -> {
            if (running) {
                log.line("MACHINE policy " + (admit ? "admit" : "refuse")
                        + " status=" + Mcl.machinePolicy(handle, admit));
            }
        });
    }

    private void receive(int transport, byte[] bytes) {
        if (!running) {
            return;
        }
        final int status = Mcl.machineReceive(handle, transport, bytes, bytes.length);
        if (status != 0) {
            log.line("MACHINE receive status=" + status + " transport=" + transport);
        }
        service.run();
    }

    private final Runnable service = new Runnable() {
        @Override public void run() {
            if (!running) {
                return;
            }
            Mcl.machineSetMediumState(handle, mediumBusy, selfTransmitting);
            final int[] event = new int[6];
            final int status = Mcl.machinePoll(handle, event);
            if (status != 0) {
                log.line("MACHINE poll status=" + status);
            }
            if (event[0] != 0) {
                log.line("MACHINE event=" + event[0]
                        + " transport=" + event[1]
                        + " profile=" + event[2]
                        + " peer_ref=" + String.format("%08X", event[3])
                        + " session_ref=" + String.format("%08X", event[4])
                        + " status=" + event[5]
                        + (event[0] == EVENT_POLICY_REQUIRED
                           ? " ACTION=machine admit|refuse" : ""));
            }
            try {
                serviceCandidate();
            } catch (RuntimeException failure) {
                candidateOpening = false;
                log.line("MACHINE candidate exception=" + failure
                        + " refused_status=" + Mcl.machineCandidateRefused(handle));
            }
            work.removeCallbacks(this);
            work.postDelayed(this, SERVICE_MS);
        }
    };

    private void serviceCandidate() {
        final int[] request = new int[5];
        if (Mcl.machineTakeCandidateRequest(handle, request) > 0) {
            if (request[4] != 0) {
                candidateOpening = false;
                ble.stop();
                log.line("MACHINE candidate closed transport=" + request[0]);
            } else if (request[0] != TRANSPORT_BLE) {
                log.line("MACHINE candidate REFUSED transport=" + request[0]);
                log.line("MACHINE candidate refused status="
                        + Mcl.machineCandidateRefused(handle));
            } else {
                candidateOpening = true;
                final boolean initiated = ble.start() && (request[2] != 0
                        ? ble.scanFor(request[2])
                        : request[3] != 0 && ble.advertise(request[3]));
                log.line("MACHINE BLE-ACTIVATE "
                        + (request[2] != 0 ? "scan" : "advertise")
                        + " initiated=" + initiated);
                if (!initiated) {
                    candidateOpening = false;
                    log.line("MACHINE candidate refused status="
                            + Mcl.machineCandidateRefused(handle));
                }
            }
        }
        if (candidateOpening && ble.isLinked()) {
            candidateOpening = false;
            log.line("MACHINE candidate ready status="
                    + Mcl.machineCandidateReady(handle));
        }
    }

    private void startListenerThread() {
        listenerThread = new Thread(() -> {
            final short[] block = new short[2048];
            while (listening) {
                final int got = audio.read(block);
                if (got > 0) {
                    Mcl.listenerPush(listener, block, got);
                }
                final byte[] payload = new byte[17];
                final int[] info = new int[5];
                final int result = Mcl.listenerPoll(listener, payload, info);
                /* WAITING means a preamble is acquired and its body is still
                   on the shared medium: the same carrier-sense fact used by
                   the embedded reference node. */
                mediumBusy = result == 3;
                if (result == Mcl.LISTEN_CONTACT && !selfTransmitting) {
                    final byte[] exact = Arrays.copyOf(payload, info[0]);
                    work.post(() -> receive(TRANSPORT_AP, exact));
                }
            }
        }, "mcl-machine-listener");
        listenerThread.start();
    }

    @Override
    public void close() {
        if (closed) {
            return;
        }
        closed = true;
        running = false;
        listening = false;
        work.removeCallbacks(service);
        audio.closeMicrophone();
        if (listenerThread != null && listenerThread != Thread.currentThread()) {
            try {
                listenerThread.join(2000L);
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
            }
        }
        if (listener != 0L) {
            Mcl.listenerDestroy(listener);
            listener = 0L;
        }
        ble.stop();
        Mcl.machineDestroy(handle);
    }
}
