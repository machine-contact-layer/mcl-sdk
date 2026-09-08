package org.mcl.bench;

/**
 * The canonical MCL C, as this app sees it.
 *
 * <p>Every method here is implemented by {@code mcl_jni.c} over the sources
 * staged from mcl-wire, mcl-link and mcl-ap at build time. <b>Nothing in this
 * package re-implements any of it.</b> A modulator written in Java would be a
 * second implementation, and a band measured with two implementations that
 * disagree is a measurement of the disagreement.
 */
public final class Mcl {

    static {
        System.loadLibrary("mclbench");
    }

    private Mcl() {
    }

    /** Majors and sizes, for the run record. */
    public static native String version();

    /**
     * Modulate one payload into {@code out}. Returns the sample count, or a
     * negative modem status. Band arguments of 0 mean the modem default.
     */
    public static native int modulate(byte[] payload, short[] out,
                                      int bandLow, int bandHigh);

    /** Exactly how many samples {@link #modulate} will write. */
    public static native int modulatedSamples(int payloadBytes,
                                              int bandLow, int bandHigh);

    /** Opaque handle, or 0. The window is grown to the profile minimum. */
    public static native long listenerCreate(int maxPayload, int bandLow,
                                             int bandHigh, int windowSamples);

    public static native void listenerDestroy(long handle);

    /** Offer captured audio. Does no correlation, so it is safe on the audio thread. */
    public static native int listenerPush(long handle, short[] pcm, int count);

    /**
     * Search what has arrived and report one result:
     * 0 QUIET, 1 CONTACT, 2 HEARD, 3 WAITING.
     *
     * <p>{@code info} receives
     * {payloadBytes, modemStatus, samplesUnscanned, contacts, heard}.
     * {@code samplesUnscanned} is the number that says a listener was too busy
     * to hear, which is otherwise indistinguishable from a quiet room.
     */
    public static native int listenerPoll(long handle, byte[] outPayload, int[] info);

    /* The three objects AP-BOOTSTRAP-1 carries, at the Stable major. */
    public static native byte[] encodePresence(int sourceRef, int capabilityTag, int ttl);

    public static native byte[] encodeTransportOffer(int sourceRef, int migrationRef,
                                                     int transportId, int profileId,
                                                     int endpointToken, int validity);

    public static native byte[] encodeTransportAccept(int sourceRef, int migrationRef,
                                                      int transportId, int profileId,
                                                      int sessionRef);

    /** A description, or null when the bytes are not a canonical object. */
    public static native String describeTier0(byte[] payload, int length);

    public static final int LISTEN_QUIET = 0;
    public static final int LISTEN_CONTACT = 1;
    public static final int LISTEN_HEARD = 2;
    public static final int LISTEN_WAITING = 3;
}
