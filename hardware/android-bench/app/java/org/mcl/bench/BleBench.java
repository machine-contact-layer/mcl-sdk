package org.mcl.bench;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattServer;
import android.bluetooth.BluetoothGattServerCallback;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.le.AdvertiseCallback;
import android.bluetooth.le.AdvertiseData;
import android.bluetooth.le.AdvertiseSettings;
import android.bluetooth.le.BluetoothLeAdvertiser;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.Context;
import android.os.ParcelUuid;

import java.util.UUID;

/**
 * BLE-ACTIVATE-1 and BLE-GATT-1 on Android.
 *
 * <p><b>Roles are not chosen here.</b> Only {@code TRANSPORT_OFFER} carries an
 * {@code endpoint_token}, and it is the offerer's own, so the offerer is the
 * only peer that can be <i>found</i>: it advertises and is the GATT
 * peripheral, and the acceptor scans for that token and connects as central.
 * Any other assignment needs information the wire does not carry.
 *
 * <p><b>The advertisement is exactly one structure.</b> {@code AdvertiseData}
 * is built with {@code setIncludeDeviceName(false)} and
 * {@code setIncludeTxPowerLevel(false)}, and carries service data for the MCL
 * 128-bit UUID and nothing else. Android adds the flags octet itself, which is
 * required by the Bluetooth specification for a connectable advertisement and
 * is not an MCL structure. Anything more would be a different advertisement,
 * and a legacy PDU has 31 bytes to spend.
 *
 * <p>The scanner matches on the UUID <b>and</b> the full eight-byte beacon,
 * from {@link ScanResult#getScanRecord()}'s raw bytes rather than from the
 * parsed helper, so what is checked is what is on the air.
 */
public final class BleBench {

    public static final UUID SERVICE_UUID =
            UUID.fromString("6d636c00-0001-4d43-4c00-6d636c626c65");
    public static final UUID RX_CHAR_UUID =
            UUID.fromString("6d636c00-0002-4d43-4c00-6d636c626c65");
    public static final UUID TX_CHAR_UUID =
            UUID.fromString("6d636c00-0003-4d43-4c00-6d636c626c65");
    private static final UUID CCCD_UUID =
            UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");

    /** BLE-GATT-1 section 3.1. One header byte: START | END | 6-bit sequence. */
    private static final int FRAG_START = 0x80;
    private static final int FRAG_END = 0x40;
    private static final int FRAG_SEQ_MASK = 0x3F;
    private static final int ATT_DEFAULT_MTU = 23;
    private static final int ATT_HEADER = 3;

    public interface Logger {
        void line(String text);
    }

    public interface FrameSink {
        void frame(byte[] frame);
    }

    private final Context context;
    private final Logger log;
    private FrameSink sink;
    private volatile boolean scanActive;

    private BluetoothAdapter adapter;
    private BluetoothLeAdvertiser advertiser;
    private BluetoothLeScanner scanner;
    private BluetoothGattServer server;
    private BluetoothGattCharacteristic txCharacteristic;
    private BluetoothDevice connectedCentral;
    private BluetoothGatt clientGatt;
    private BluetoothGattCharacteristic remoteRx;
    private volatile boolean candidateReady;

    private byte[] wantedBeacon;
    private int scanMatches;
    private int scanUuidOnly;
    private String lastRawRecord = "";

    /* Reassembly state, one frame at a time: this protocol is sequential on
       the candidate and a queue would only hide a scheduling bug. */
    private byte[] reassembly = new byte[1048];
    private int reassemblyLength;
    private int expectSequence = -1;

    public BleBench(Context context, Logger log) {
        this.context = context;
        this.log = log;
    }

    public void setFrameSink(FrameSink sink) {
        this.sink = sink;
    }

    public boolean start() {
        final BluetoothManager manager =
                (BluetoothManager) context.getSystemService(Context.BLUETOOTH_SERVICE);
        if (manager == null) {
            log.line("BLE refused: no Bluetooth service");
            return false;
        }
        adapter = manager.getAdapter();
        if (adapter == null || !adapter.isEnabled()) {
            log.line("BLE refused: adapter absent or off");
            return false;
        }
        advertiser = adapter.getBluetoothLeAdvertiser();
        scanner = adapter.getBluetoothLeScanner();
        log.line("BLE ready advertiser=" + (advertiser != null)
                 + " scanner=" + (scanner != null)
                 + " multi_adv=" + adapter.isMultipleAdvertisementSupported());
        return true;
    }

    /* ------------------------------------------------- BLE-ACTIVATE-1 §3 */

    /** {@code 00 00 00 00 | token[31:24] | [23:16] | [15:8] | [7:0]}. */
    public static byte[] beaconFor(int token) {
        return new byte[] {
                0, 0, 0, 0,
                (byte) ((token >>> 24) & 0xFF),
                (byte) ((token >>> 16) & 0xFF),
                (byte) ((token >>> 8) & 0xFF),
                (byte) (token & 0xFF)
        };
    }

    private final AdvertiseCallback advertiseCallback = new AdvertiseCallback() {
        @Override
        public void onStartSuccess(AdvertiseSettings settingsInEffect) {
            log.line("BLE advertising started, tx_power="
                     + settingsInEffect.getTxPowerLevel());
        }

        @Override
        public void onStartFailure(int errorCode) {
            log.line("BLE advertising REFUSED, error=" + errorCode);
        }
    };

    /** The offerer: advertise the token, and serve GATT. */
    public boolean advertise(int token) {
        if (advertiser == null) {
            log.line("BLE advertise refused: this device has no advertiser");
            return false;
        }
        if (token == 0) {
            /* BLE-ACTIVATE-1 §3: zero names nothing, because this profile's
               discovery IS the token. */
            log.line("BLE advertise REFUSED: a zero endpoint_token names nothing");
            return false;
        }
        startGattServer();

        final AdvertiseSettings settings = new AdvertiseSettings.Builder()
                .setAdvertiseMode(AdvertiseSettings.ADVERTISE_MODE_LOW_LATENCY)
                .setTxPowerLevel(AdvertiseSettings.ADVERTISE_TX_POWER_HIGH)
                .setConnectable(true)
                .build();

        final AdvertiseData data = new AdvertiseData.Builder()
                .setIncludeDeviceName(false)
                .setIncludeTxPowerLevel(false)
                .addServiceData(new ParcelUuid(SERVICE_UUID), beaconFor(token))
                .build();

        advertiser.startAdvertising(settings, data, advertiseCallback);
        log.line("BLE advertising token=" + String.format("%08X", token)
                 + " beacon=" + hex(beaconFor(token)));
        return true;
    }

    public void stopAdvertising() {
        if (advertiser != null) {
            advertiser.stopAdvertising(advertiseCallback);
            log.line("BLE advertising stopped");
        }
    }

    /* ------------------------------------------------------------- scan */

    private final ScanCallback scanCallback = new ScanCallback() {
        @Override
        public void onScanResult(int callbackType, ScanResult result) {
            if (!scanActive || result.getScanRecord() == null) {
                return;
            }
            final byte[] raw = result.getScanRecord().getBytes();
            if (raw == null) {
                return;
            }
            final boolean[] uuidSeen = new boolean[1];
            if (!matches(raw, wantedBeacon, uuidSeen)) {
                if (uuidSeen[0]) {
                    scanUuidOnly++;
                    log.line("BLE scan: MCL service, wrong transaction (uuid only)");
                }
                return;
            }
            scanMatches++;
            lastRawRecord = hex(raw);
            log.line("BLE scan MATCH addr=" + result.getDevice().getAddress()
                     + " rssi=" + result.getRssi() + " raw=" + lastRawRecord);
            onMatch(result.getDevice());
        }

        @Override
        public void onScanFailed(int errorCode) {
            log.line("BLE scan REFUSED, error=" + errorCode);
        }
    };

    /**
     * Walk the raw advertising payload. Deliberately not
     * {@code ScanRecord.getServiceData()}: the parsed helper would answer for
     * a structure that merely resembles the normative one, and what two
     * independent implementations have to agree on is the exact bytes.
     */
    public static boolean matches(byte[] payload, byte[] wantBeacon, boolean[] uuidSeen) {
        final byte[] uuidLe = serviceUuidLittleEndian();
        int i = 0;
        uuidSeen[0] = false;
        while (i + 1 < payload.length) {
            final int fieldLen = payload[i] & 0xFF;
            if (fieldLen == 0 || i + 1 + fieldLen > payload.length) {
                break;
            }
            final int type = payload[i + 1] & 0xFF;
            final int valueOffset = i + 2;
            final int valueLen = fieldLen - 1;
            if (type == 0x21 && valueLen >= 16) {
                boolean sameUuid = true;
                for (int k = 0; k < 16; k++) {
                    if (payload[valueOffset + k] != uuidLe[k]) {
                        sameUuid = false;
                        break;
                    }
                }
                if (sameUuid) {
                    uuidSeen[0] = true;
                    if (wantBeacon != null && valueLen == 16 + wantBeacon.length) {
                        boolean sameBeacon = true;
                        for (int k = 0; k < wantBeacon.length; k++) {
                            if (payload[valueOffset + 16 + k] != wantBeacon[k]) {
                                sameBeacon = false;
                                break;
                            }
                        }
                        if (sameBeacon) {
                            return true;
                        }
                    }
                }
            }
            i += 1 + fieldLen;
        }
        return false;
    }

    /** MCL service UUID 6d636c00-0001-4d43-4c00-6d636c626c65, little-endian. */
    private static byte[] serviceUuidLittleEndian() {
        final byte[] be = new byte[] {
                (byte) 0x6d, (byte) 0x63, (byte) 0x6c, (byte) 0x00,
                (byte) 0x00, (byte) 0x01, (byte) 0x4d, (byte) 0x43,
                (byte) 0x4c, (byte) 0x00, (byte) 0x6d, (byte) 0x63,
                (byte) 0x6c, (byte) 0x62, (byte) 0x6c, (byte) 0x65
        };
        final byte[] le = new byte[16];
        for (int i = 0; i < 16; i++) {
            le[i] = be[15 - i];
        }
        return le;
    }

    /** The acceptor: scan for one token, and connect to nothing else. */
    public boolean scanFor(int token) {
        if (scanner == null) {
            log.line("BLE scan refused: no scanner");
            return false;
        }
        stopScan();
        wantedBeacon = beaconFor(token);
        scanMatches = 0;
        scanUuidOnly = 0;
        /*
         * Unfiltered, and matched in software.
         *
         * A ScanFilter on service data would work on Android, and would then
         * be the only implementation doing it in the controller -- so this
         * matches the same way every other peer must, and the raw record is
         * kept as evidence rather than trusted to a platform parser.
         */
        final ScanSettings settings = new ScanSettings.Builder()
                .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
                .setCallbackType(ScanSettings.CALLBACK_TYPE_ALL_MATCHES)
                .build();
        scanActive = true;
        scanner.startScan(null, settings, scanCallback);
        log.line("BLE scanning for token=" + String.format("%08X", token)
                 + " beacon=" + hex(wantedBeacon));
        return true;
    }

    public void stopScan() {
        scanActive = false;
        if (scanner != null) {
            scanner.stopScan(scanCallback);
            log.line("BLE scan stopped, matches=" + scanMatches
                     + " uuid_only=" + scanUuidOnly);
        }
    }

    private void onMatch(BluetoothDevice device) {
        stopScan();
        connectAsCentral(device);
    }

    /* ----------------------------------------------------- GATT: server */

    private final BluetoothGattServerCallback serverCallback =
            new BluetoothGattServerCallback() {
        @Override
        public void onConnectionStateChange(BluetoothDevice device, int status, int newState) {
            candidateReady = false;
            if (status == BluetoothGatt.GATT_SUCCESS && newState == BluetoothGatt.STATE_CONNECTED) {
                connectedCentral = device;
                resetReassembly();
                log.line("BLE central connected: " + device.getAddress());
            } else {
                connectedCentral = null;
                log.line("BLE central disconnected");
            }
        }

        @Override
        public void onCharacteristicWriteRequest(BluetoothDevice device, int requestId,
                                                 BluetoothGattCharacteristic characteristic,
                                                 boolean preparedWrite, boolean responseNeeded,
                                                 int offset, byte[] value) {
            log.line("BLE ATT write uuid=" + characteristic.getUuid()
                     + " offset=" + offset + " prepared=" + preparedWrite
                     + " response=" + responseNeeded + " bytes="
                     + (value == null ? "null" : hex(value)));
            if (RX_CHAR_UUID.equals(characteristic.getUuid())) {
                acceptFragment(value);
            }
            if (responseNeeded && server != null) {
                server.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS,
                                    offset, null);
            }
        }

        @Override
        public void onDescriptorWriteRequest(BluetoothDevice device, int requestId,
                                             BluetoothGattDescriptor descriptor,
                                             boolean preparedWrite, boolean responseNeeded,
                                             int offset, byte[] value) {
            log.line("BLE ATT descriptor uuid=" + descriptor.getUuid()
                     + " offset=" + offset + " prepared=" + preparedWrite
                     + " response=" + responseNeeded + " bytes="
                     + (value == null ? "null" : hex(value)));
            final boolean enable = CCCD_UUID.equals(descriptor.getUuid())
                    && TX_CHAR_UUID.equals(descriptor.getCharacteristic().getUuid())
                    && !preparedWrite && offset == 0 && value != null
                    && value.length == 2 && value[0] == 1 && value[1] == 0;
            boolean acknowledged = server != null;
            if (responseNeeded && server != null) {
                acknowledged = server.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS,
                                                    offset, null);
            }
            candidateReady = enable && acknowledged && device.equals(connectedCentral);
            if (candidateReady) {
                log.line("BLE candidate ready as peripheral after notification subscription");
            }
        }
    };

    private void startGattServer() {
        if (server != null) {
            return;
        }
        final BluetoothManager manager =
                (BluetoothManager) context.getSystemService(Context.BLUETOOTH_SERVICE);
        server = manager.openGattServer(context, serverCallback);
        if (server == null) {
            log.line("BLE GATT server refused");
            return;
        }
        final BluetoothGattService service = new BluetoothGattService(
                SERVICE_UUID, BluetoothGattService.SERVICE_TYPE_PRIMARY);

        final BluetoothGattCharacteristic rx = new BluetoothGattCharacteristic(
                RX_CHAR_UUID,
                BluetoothGattCharacteristic.PROPERTY_WRITE
                        | BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE,
                BluetoothGattCharacteristic.PERMISSION_WRITE);

        txCharacteristic = new BluetoothGattCharacteristic(
                TX_CHAR_UUID,
                BluetoothGattCharacteristic.PROPERTY_NOTIFY,
                BluetoothGattCharacteristic.PERMISSION_READ);
        txCharacteristic.addDescriptor(new BluetoothGattDescriptor(
                CCCD_UUID,
                BluetoothGattDescriptor.PERMISSION_READ
                        | BluetoothGattDescriptor.PERMISSION_WRITE));

        service.addCharacteristic(rx);
        service.addCharacteristic(txCharacteristic);
        server.addService(service);
        log.line("BLE GATT server up, MCL-RX write / MCL-TX notify");
    }

    /* ----------------------------------------------------- GATT: client */

    private final BluetoothGattCallback clientCallback = new BluetoothGattCallback() {
        @Override
        public void onConnectionStateChange(BluetoothGatt gatt, int status, int newState) {
            if (gatt != clientGatt) { return; }
            candidateReady = false;
            if (status == BluetoothGatt.GATT_SUCCESS && newState == BluetoothGatt.STATE_CONNECTED) {
                log.line("BLE connected as central, discovering");
                resetReassembly();
                gatt.discoverServices();
            } else {
                remoteRx = null;
                log.line("BLE central link down, status=" + status);
            }
        }

        @Override
        public void onServicesDiscovered(BluetoothGatt gatt, int status) {
            if (gatt != clientGatt || status != BluetoothGatt.GATT_SUCCESS) { return; }
            final BluetoothGattService service = gatt.getService(SERVICE_UUID);
            if (service == null) {
                log.line("BLE peer has no MCL service");
                return;
            }
            remoteRx = service.getCharacteristic(RX_CHAR_UUID);
            final BluetoothGattCharacteristic remoteTx =
                    service.getCharacteristic(TX_CHAR_UUID);
            if (remoteRx == null || remoteTx == null) {
                log.line("BLE peer is missing a characteristic");
                return;
            }
            if (!gatt.setCharacteristicNotification(remoteTx, true)) { return; }
            final BluetoothGattDescriptor cccd = remoteTx.getDescriptor(CCCD_UUID);
            if (cccd != null) {
                cccd.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
                if (!gatt.writeDescriptor(cccd)) { log.line("BLE subscription write refused"); }
            }
        }

        @Override
        public void onDescriptorWrite(BluetoothGatt gatt, BluetoothGattDescriptor descriptor, int status) {
            if (gatt != clientGatt || !CCCD_UUID.equals(descriptor.getUuid())) { return; }
            candidateReady = status == BluetoothGatt.GATT_SUCCESS && remoteRx != null;
            log.line("BLE candidate subscription status=" + status + " ready=" + candidateReady);
        }

        @Override
        public void onCharacteristicChanged(BluetoothGatt gatt,
                                            BluetoothGattCharacteristic characteristic) {
            if (gatt == clientGatt && TX_CHAR_UUID.equals(characteristic.getUuid())) {
                acceptFragment(characteristic.getValue());
            }
        }
    };

    private void connectAsCentral(BluetoothDevice device) {
        clientGatt = device.connectGatt(context, false, clientCallback,
                                        BluetoothDevice.TRANSPORT_LE);
        log.line("BLE connecting to " + device.getAddress());
    }

    /* ------------------------------------------------ BLE-GATT-1 carriage */

    private void resetReassembly() {
        reassemblyLength = 0;
        expectSequence = -1;
    }

    private void acceptFragment(byte[] pdu) {
        if (pdu == null || pdu.length == 0) {
            return;
        }
        final int header = pdu[0] & 0xFF;
        final int seq = header & FRAG_SEQ_MASK;
        final boolean start = (header & FRAG_START) != 0;
        final boolean end = (header & FRAG_END) != 0;

        if (start) {
            reassemblyLength = 0;
            expectSequence = seq;
        } else if (expectSequence < 0 || seq != ((expectSequence + 1) % 64)) {
            /* "A receiver MUST refuse a fragment whose sequence is not the
               expected successor." Splicing unrelated bytes into a frame is
               the failure reassembly cannot detect afterwards. */
            log.line("BLE fragment out of sequence: " + seq + ", frame discarded");
            resetReassembly();
            return;
        } else {
            expectSequence = seq;
        }

        final int payload = pdu.length - 1;
        if (reassemblyLength + payload > reassembly.length) {
            log.line("BLE reassembly overflow, frame discarded");
            resetReassembly();
            return;
        }
        System.arraycopy(pdu, 1, reassembly, reassemblyLength, payload);
        reassemblyLength += payload;

        if (end) {
            final byte[] frame = new byte[reassemblyLength];
            System.arraycopy(reassembly, 0, frame, 0, reassemblyLength);
            resetReassembly();
            log.line("BLE frame in, " + frame.length + " bytes");
            if (sink != null) {
                sink.frame(frame);
            }
        }
    }

    /**
     * Send one frame, fragmented at the SMALLEST MTU BLE permits rather than
     * at whatever this connection negotiated. A peer that negotiates a large
     * MTU would carry every frame in one PDU and leave the fragmentation path
     * completely untested; the minimum is the case the scheme has to survive.
     */
    public boolean sendFrame(byte[] frame) {
        if (!candidateReady) {
            log.line("BLE send refused: candidate is not subscribed");
            return false;
        }
        final int per = ATT_DEFAULT_MTU - ATT_HEADER - 1;
        final int total = Math.max(1, (frame.length + per - 1) / per);
        if (total > 64) {
            log.line("BLE frame needs " + total + " fragments, more than the "
                     + "6-bit sequence can carry without wrapping");
            return false;
        }
        for (int i = 0; i < total; i++) {
            final int offset = i * per;
            final int length = Math.min(per, frame.length - offset);
            final byte[] pdu = new byte[length + 1];
            int header = i & FRAG_SEQ_MASK;
            if (i == 0) {
                header |= FRAG_START;
            }
            if (i == total - 1) {
                header |= FRAG_END;
            }
            pdu[0] = (byte) header;
            System.arraycopy(frame, offset, pdu, 1, length);

            if (!writeOnePdu(pdu)) {
                return false;
            }
            try {
                Thread.sleep(20);
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
            }
        }
        return true;
    }

    private boolean writeOnePdu(byte[] pdu) {
        if (clientGatt != null && remoteRx != null) {
            remoteRx.setValue(pdu);
            remoteRx.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE);
            return clientGatt.writeCharacteristic(remoteRx);
        }
        if (server != null && txCharacteristic != null && connectedCentral != null) {
            txCharacteristic.setValue(pdu);
            return server.notifyCharacteristicChanged(connectedCentral,
                                                      txCharacteristic, false);
        }
        log.line("BLE send refused: no link in either role");
        return false;
    }

    public void stop() {
        candidateReady = false;
        stopAdvertising();
        stopScan();
        if (clientGatt != null) {
            clientGatt.disconnect();
            clientGatt.close();
            clientGatt = null;
        }
        if (server != null) {
            server.close();
            server = null;
        }
        remoteRx = null;
        txCharacteristic = null;
        connectedCentral = null;
    }

    public int scanMatches() {
        return scanMatches;
    }

    public int scanUuidOnly() {
        return scanUuidOnly;
    }

    public String lastRawRecord() {
        return lastRawRecord;
    }

    public boolean isLinked() {
        return candidateReady;
    }

    public static String hex(byte[] data) {
        final StringBuilder out = new StringBuilder(data.length * 2);
        for (byte b : data) {
            out.append(String.format("%02X", b));
        }
        return out.toString();
    }
}
