package org.mcl.bench;

import android.Manifest;
import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * The MCL Android bench.
 *
 * <p><b>LAB INSTRUMENT.</b> Not a product, not a service, not an Android port
 * of MCL in any sense a builder should copy for a shipping app: it holds the
 * screen on, does its work on one background thread, and keeps everything in
 * memory. What it is for is being the <i>second machine</i> — a commodity
 * handset with a speaker, a microphone and a BLE radio that nobody on this
 * project designed — so that measurements which currently come from one
 * transmitter class can come from two.
 *
 * <p><b>The control plane is not the data plane.</b> Commands arrive as
 * broadcasts and results go to logcat, so a campaign can be driven from a
 * laptop over adb. adb is deployment and instrumentation; every MCL byte
 * crosses the air. There is deliberately no command that carries a peer
 * address, a source_ref, a session reference, a secret or a pairing state —
 * and where a command DOES carry a token, the run says so, because a token
 * that came from the operator is not a token learned from the air.
 *
 * <pre>
 * adb shell am broadcast -a org.mcl.bench.CMD --es cmd "emit PRESENCE"
 * adb logcat -s MCLBENCH
 * </pre>
 */
public final class BenchActivity extends Activity {

    public static final String TAG = "MCLBENCH";
    private static final String ACTION_CMD = "org.mcl.bench.CMD";

    private TextView view;
    private ScrollView scroller;
    private HandlerThread worker;
    private Handler work;
    private Handler ui;

    private AudioBench audio;
    private BleBench ble;
    private MachineAdapter machine;

    private int bandLow;
    private int bandHigh;
    private int sourceRef;
    private long listener;
    private volatile boolean listening;

    private final List<String> lines = new ArrayList<>();

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        scroller = new ScrollView(this);
        view = new TextView(this);
        view.setTypeface(Typeface.MONOSPACE);
        view.setTextSize(10.0f);
        view.setTextColor(Color.BLACK);
        view.setBackgroundColor(Color.WHITE);
        view.setPadding(16, 16, 16, 16);
        scroller.addView(view, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        setContentView(scroller);

        ui = new Handler(getMainLooper());
        worker = new HandlerThread("mcl-bench");
        worker.start();
        work = new Handler(worker.getLooper());

        audio = new AudioBench(this::log);
        ble = new BleBench(this, this::log);
        /*
         * Frames arriving on a BLE link are reported and echoed. The echo is
         * the point of the diagnostic: it exercises the notify direction and
         * the fragmenter, so a peer can check that a frame survives the round
         * trip at the minimum MTU rather than only that a write was accepted.
         * The full rendezvous replaces this sink with the machine's own.
         */
        ble.setFrameSink(frame -> {
            log("BLE frame in " + frame.length + " bytes: " + BleBench.hex(frame));
            final String described = Mcl.describeTier0(frame, frame.length);
            if (described != null) {
                log("  which decodes as " + described);
            }
            work.post(() -> log("BLE echo " + (ble.sendFrame(frame) ? "sent" : "REFUSED")));
        });

        /*
         * A source_ref is a correlation reference with no uniqueness property
         * and no identity meaning. Randomised per launch so two runs are
         * distinguishable, and NOT taken from any device identifier: a
         * hardware id here would make every capture carry one.
         */
        sourceRef = (int) (System.nanoTime() ^ (System.nanoTime() << 17));
        if (sourceRef == 0) {
            sourceRef = 0x4D434C31;
        }

        registerReceiver(commands, new IntentFilter(ACTION_CMD),
                         Context.RECEIVER_EXPORTED);

        /*
         * ACCESS_FINE_LOCATION is declared with maxSdkVersion 30 and is NOT
         * requested here. Asking for a permission the manifest does not carry
         * on this API level is refused by the platform, and the refusal looks
         * like a permission problem rather than a manifest one.
         */
        requestPermissions(new String[] {
                Manifest.permission.RECORD_AUDIO,
                Manifest.permission.BLUETOOTH_SCAN,
                Manifest.permission.BLUETOOTH_ADVERTISE,
                Manifest.permission.BLUETOOTH_CONNECT
        }, 1);

        log("MCL bench up");
        log("native: " + Mcl.version());
        log("source_ref=" + String.format("%08X", sourceRef) + " provenance=LOCAL");
        log("commands: adb shell am broadcast -a " + ACTION_CMD + " --es cmd \"...\"");
        log("  emit <PRESENCE|OFFER|ACCEPT> [gain_pct]");
        log("  ladder <lo> <hi> [reps]");
        log("  band <lo> <hi>    listen <ms>    stop");
        log("  ble on|off    adv <tokenhex>    scan <tokenhex>    blesend <hex>");
        log("  machine start <initiator|responder> | admit | refuse | stop");
    }

    @Override
    protected void onDestroy() {
        unregisterReceiver(commands);
        if (machine != null) {
            final MachineAdapter closing = machine;
            machine = null;
            final CountDownLatch closed = new CountDownLatch(1);
            work.post(() -> {
                try {
                    closing.close();
                } finally {
                    closed.countDown();
                }
            });
            try {
                if (!closed.await(3000L, TimeUnit.MILLISECONDS)) {
                    log("MACHINE shutdown timed out");
                }
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
            }
        }
        stopListening();
        if (ble != null) {
            ble.stop();
        }
        worker.quitSafely();
        super.onDestroy();
    }

    /* ------------------------------------------------------------- log */

    void log(String text) {
        Log.i(TAG, text);
        ui.post(() -> {
            lines.add(text);
            while (lines.size() > 400) {
                lines.remove(0);
            }
            view.setText(String.join("\n", lines));
            scroller.fullScroll(ScrollView.FOCUS_DOWN);
        });
    }

    /* --------------------------------------------------------- commands */

    private final BroadcastReceiver commands = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            final String cmd = intent.getStringExtra("cmd");
            if (cmd == null) {
                return;
            }
            log("> " + cmd);
            work.post(() -> dispatch(cmd.trim()));
        }
    };

    private void dispatch(String cmd) {
        final String[] parts = cmd.split("\\s+");
        try {
            switch (parts[0].toLowerCase()) {
                case "band":
                    bandLow = Integer.parseInt(parts[1]);
                    bandHigh = Integer.parseInt(parts[2]);
                    log("BAND " + bandLow + "/" + bandHigh
                        + " (preamble derived as f0-1000..f1, floor 500)");
                    break;
                case "emit":
                    emit(parts[1], parts.length > 2 ? Integer.parseInt(parts[2]) : 100);
                    break;
                case "ladder":
                    ladder(Integer.parseInt(parts[1]), Integer.parseInt(parts[2]),
                           parts.length > 3 ? Integer.parseInt(parts[3]) : 10);
                    break;
                case "listen":
                    startListening(Integer.parseInt(parts[1]));
                    break;
                case "stop":
                    stopListening();
                    break;
                case "ble":
                    if ("on".equals(parts[1])) {
                        ble.start();
                    } else {
                        ble.stop();
                    }
                    break;
                case "adv":
                    logConfiguredToken(parts[1]);
                    ble.advertise((int) Long.parseLong(parts[1], 16));
                    break;
                case "scan":
                    logConfiguredToken(parts[1]);
                    ble.scanFor((int) Long.parseLong(parts[1], 16));
                    break;
                case "blesend":
                    ble.sendFrame(unhex(parts[1]));
                    break;
                case "blestat":
                    log("BLE matches=" + ble.scanMatches()
                        + " uuid_only=" + ble.scanUuidOnly()
                        + " linked=" + ble.isLinked()
                        + " last_raw=" + ble.lastRawRecord());
                    break;
                case "machine":
                    machineCommand(parts);
                    break;
                default:
                    log("unknown command");
                    break;
            }
        } catch (RuntimeException bad) {
            log("command refused: " + bad);
        }
    }

    private void machineCommand(String[] parts) {
        if (parts.length < 2) {
            throw new IllegalArgumentException("machine action required");
        }
        switch (parts[1].toLowerCase()) {
            case "start":
                if (parts.length < 3) {
                    throw new IllegalArgumentException("machine role required");
                }
                if (machine != null) {
                    throw new IllegalStateException("machine already running");
                }
                stopListening();
                final int role;
                if ("initiator".equalsIgnoreCase(parts[2])) {
                    role = 0;
                } else if ("responder".equalsIgnoreCase(parts[2])) {
                    role = 1;
                } else {
                    throw new IllegalArgumentException("role must be initiator or responder");
                }
                machine = new MachineAdapter(work, audio, ble, this::log,
                        sourceRef, role, bandLow, bandHigh);
                ble.setFrameSink(machine::receiveBle);
                machine.start();
                break;
            case "admit":
                requireMachine().policy(true);
                break;
            case "refuse":
                requireMachine().policy(false);
                break;
            case "stop":
                requireMachine().close();
                machine = null;
                installDiagnosticBleSink();
                log("MACHINE stopped");
                break;
            default:
                throw new IllegalArgumentException("unknown machine action");
        }
    }

    private MachineAdapter requireMachine() {
        if (machine == null) {
            throw new IllegalStateException("machine is not running");
        }
        return machine;
    }

    private void installDiagnosticBleSink() {
        ble.setFrameSink(frame -> {
            log("BLE frame in " + frame.length + " bytes: " + BleBench.hex(frame));
            final String described = Mcl.describeTier0(frame, frame.length);
            if (described != null) {
                log("  which decodes as " + described);
            }
            work.post(() -> log("BLE echo "
                    + (ble.sendFrame(frame) ? "sent" : "REFUSED")));
        });
    }

    /**
     * A token supplied by the operator is CONFIGURED, and every run that uses
     * one says so. In a real zero-prior exchange the token arrives in a
     * TRANSPORT_OFFER off the air; these commands exist to check that two
     * implementations encode the same advertising bytes, which is a different
     * question.
     */
    private void logConfiguredToken(String token) {
        log("VALUE endpoint_token=" + token.toUpperCase()
            + " provenance=CONFIGURED (this run is NOT zero-prior)");
    }

    /* ------------------------------------------------------------ emit */

    private byte[] object(String which) {
        switch (which.toUpperCase()) {
            case "PRESENCE":
                return Mcl.encodePresence(sourceRef, 1, 60);
            case "OFFER":
                /* transport 3 = BLE, profile 1 = BLE-GATT, a token that is
                   this run's, and 30 s of validity. */
                return Mcl.encodeTransportOffer(sourceRef, 0x4D194201, 3, 1,
                                                0xA5C30F17, 30);
            case "ACCEPT":
                return Mcl.encodeTransportAccept(sourceRef, 0x4D194201, 3, 1,
                                                 0x5E5510C7);
            default:
                return null;
        }
    }

    private void emit(String which, int gainPercent) {
        final byte[] payload = object(which);
        if (payload == null) {
            log("emit refused: unknown object");
            return;
        }
        final int samples = Mcl.modulatedSamples(payload.length, bandLow, bandHigh);
        if (samples <= 0) {
            log("emit refused: modulator reports " + samples + " samples");
            return;
        }
        final short[] pcm = new short[samples];
        final int used = Mcl.modulate(payload, pcm, bandLow, bandHigh);
        if (used <= 0) {
            log("emit refused: modulate rc=" + used);
            return;
        }
        log("EMIT " + which.toUpperCase() + " " + payload.length + " bytes, "
            + used + " samples, band " + bandLow + "/" + bandHigh
            + ", hex=" + BleBench.hex(payload));
        final boolean completed = audio.play(pcm, used, gainPercent / 100.0f);
        log("EMIT COMPLETE " + which.toUpperCase() + " ok=" + completed);
    }

    /**
     * One rung of the band ladder: emit the same object repeatedly at one
     * band, with a gap, so the peer can count recoveries. The repetitions are
     * what makes a rung a measurement rather than an anecdote.
     */
    private void ladder(int low, int high, int repetitions) {
        bandLow = low;
        bandHigh = high;
        log("LADDER band=" + low + "/" + high + " reps=" + repetitions);
        for (int i = 0; i < repetitions; i++) {
            log("LADDER rung " + (i + 1) + " of " + repetitions);
            emit("PRESENCE", 100);
            try {
                Thread.sleep(1500);
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
                return;
            }
        }
        log("LADDER done band=" + low + "/" + high);
    }

    /* ---------------------------------------------------------- listen */

    private void startListening(int durationMs) {
        if (listening) {
            log("already listening");
            return;
        }
        /* 17 bytes is the largest object AP-BOOTSTRAP-1 carries. Declaring 64
           would cost about 1.4 s of extra patience on every frame that fails. */
        listener = Mcl.listenerCreate(17, bandLow, bandHigh, 96000);
        if (listener == 0) {
            log("listener refused");
            return;
        }
        if (!audio.openMicrophone()) {
            Mcl.listenerDestroy(listener);
            listener = 0;
            return;
        }
        listening = true;
        final long deadline = System.currentTimeMillis() + durationMs;
        log("LISTEN for " + durationMs + " ms, band " + bandLow + "/" + bandHigh);

        /*
         * A THREAD, NOT THE COMMAND HANDLER.
         *
         * The listen loop runs until its deadline, so posting it to the same
         * Handler that dispatches commands makes every later command -- `emit`
         * above all -- queue behind it. A rig that cannot transmit while it is
         * listening cannot run a loopback, and the failure looks like a
         * command that was ignored.
         */
        new Thread(() -> {
            final short[] block = new short[2048];
            final byte[] payload = new byte[17];
            final int[] info = new int[5];
            int contacts = 0;
            int heard = 0;

            while (listening && System.currentTimeMillis() < deadline) {
                final int got = audio.read(block);
                if (got > 0) {
                    Mcl.listenerPush(listener, block, got);
                }
                final int result = Mcl.listenerPoll(listener, payload, info);
                if (result == Mcl.LISTEN_CONTACT) {
                    contacts++;
                    final String described = Mcl.describeTier0(payload, info[0]);
                    log("CONTACT " + info[0] + " bytes: "
                        + (described != null ? described : "not a canonical object"));
                } else if (result == Mcl.LISTEN_HEARD) {
                    heard++;
                    log("HEARD (preamble, payload lost) modem_rc=" + info[1]);
                }
            }
            log("LISTEN done contacts=" + contacts + " heard=" + heard
                + " unscanned=" + info[2]);
            stopListening();
        }, "mcl-listen").start();
    }

    private void stopListening() {
        if (!listening) {
            return;
        }
        listening = false;
        audio.closeMicrophone();
        if (listener != 0) {
            Mcl.listenerDestroy(listener);
            listener = 0;
        }
    }

    private static byte[] unhex(String hex) {
        final int n = hex.length() / 2;
        final byte[] out = new byte[n];
        for (int i = 0; i < n; i++) {
            out[i] = (byte) Integer.parseInt(hex.substring(i * 2, i * 2 + 2), 16);
        }
        return out;
    }
}
