// P/Invoke surface over mcl_dual_host.dll.
//
// Every declaration here mirrors a function in tools/dual_host_shim.c and
// nothing more. There is no protocol logic in this file and there must never
// be: the point of the shim is that the migration this harness performs is the
// one mcl-sdk implements, not one C# reimplements.
//
// Status values come back unchanged, so a check can assert the exact refusal
// the specification requires rather than merely that something failed.

using System;
using System.Runtime.InteropServices;

namespace Mcl.Sdk.DualTransport
{
    // int32_t (*)(void *user, uint8_t transport_id, const uint8_t *data, size_t data_size)
    //
    // Returns 0 for accepted, negative for definitely-not-transmitted, positive
    // for an outcome the radio cannot vouch for. The three-way answer is what
    // lets the SDK decide whether a COMMIT became irrevocable.
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate int TxFn(IntPtr user, byte transportId, IntPtr data, UIntPtr size);

    [StructLayout(LayoutKind.Sequential)]
    internal struct Rx
    {
        public int FrameClass;
        public uint SourceRef;
        public uint DestinationRef;
        public int Sequence;
        public int Kind;              // -1 when the frame carried no object
        public uint MigrationRef;
        public int TransportId;
        public int ProfileId;
        public uint EndpointToken;
        public int Validity;
        public uint SessionRef;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct ContactSnapshot
    {
        public int State;
        public int Role;
        public int ActiveTransport;
        public int ControlTransport;
        public int DataTransport;
        public int Quiesced;
        public uint LocalRef;
        public uint PeerRef;
        public int PeerRefValid;
        public uint SessionRef;
        public int SessionValid;
        public uint PendingMigrationRef;
        public int PendingTransport;
        public int PendingProfile;
        public uint PendingEndpointToken;
        public uint CompletedMigrationRef;
        public int MigrationCount;
        public int LinkState;
    }

    internal static class Mcl
    {
        private const string Dll = "mcl_dual_host";
        private const CallingConvention Cdecl = CallingConvention.Cdecl;

        // ---- sizes and registry values, read from the C rather than restated ----

        [DllImport(Dll, CallingConvention = Cdecl)] internal static extern int mclx_node_size();
        [DllImport(Dll, CallingConvention = Cdecl)] internal static extern int mclx_reassembler_size();
        [DllImport(Dll, CallingConvention = Cdecl)] internal static extern int mclx_frame_max_size();
        [DllImport(Dll, CallingConvention = Cdecl)] internal static extern int mclx_ble_default_mtu();
        [DllImport(Dll, CallingConvention = Cdecl)] internal static extern int mclx_challenge_size();
        [DllImport(Dll, CallingConvention = Cdecl)] internal static extern int mclx_transport_ip();
        [DllImport(Dll, CallingConvention = Cdecl)] internal static extern int mclx_transport_ble();

        // ---- BLE carriage ----

        [DllImport(Dll, CallingConvention = Cdecl)]
        internal static extern int mclx_ble_fragment_count(int frameSize, int attMtu);

        [DllImport(Dll, CallingConvention = Cdecl)]
        internal static extern int mclx_ble_fragment(byte[] frame, int frameSize, int attMtu,
                                                     int index, byte[] outBuf, int outCapacity);

        [DllImport(Dll, CallingConvention = Cdecl)]
        internal static extern void mclx_ble_reassembler_reset(IntPtr state);

        [DllImport(Dll, CallingConvention = Cdecl)]
        internal static extern int mclx_ble_reassemble(IntPtr state, byte[] fragment, int fragmentSize,
                                                       byte[] outBuf, int outCapacity);

        // ---- IP carriage ----

        [DllImport(Dll, CallingConvention = Cdecl)]
        internal static extern int mclx_ip_datagram_validate(byte[] datagram, int size);

        // ---- node ----

        [DllImport(Dll, CallingConvention = Cdecl)]
        internal static extern int mclx_node_init(IntPtr node, uint sourceRef, int transportId,
                                                  int role, TxFn txFn, IntPtr user);

        [DllImport(Dll, CallingConvention = Cdecl)]
        internal static extern int mclx_link_transition(IntPtr node, int state);

        [DllImport(Dll, CallingConvention = Cdecl)]
        internal static extern int mclx_link_state(IntPtr node);

        // ---- send ----

        [DllImport(Dll, CallingConvention = Cdecl)]
        internal static extern int mclx_send_presence(IntPtr node, int frameClass, int flags,
                                                      uint destinationRef);

        [DllImport(Dll, CallingConvention = Cdecl)]
        internal static extern int mclx_send_transport_offer(IntPtr node, int flags,
                                                             uint destinationRef, uint migrationRef,
                                                             int transportId, int profileId,
                                                             uint endpointToken, int validity);

        // ---- receive ----

        [DllImport(Dll, CallingConvention = Cdecl)]
        internal static extern int mclx_receive_framed(IntPtr node, int arrivalTransport,
                                                       byte[] data, int size, out Rx rx);

        // ---- handoff ----

        [DllImport(Dll, CallingConvention = Cdecl)]
        internal static extern int mclx_send_handoff(IntPtr node, int operation, uint migrationRef,
                                                     uint sessionRef, byte[] challenge, int flags);

        [DllImport(Dll, CallingConvention = Cdecl)]
        internal static extern int mclx_send_handoff_raw(IntPtr node, uint sessionRef,
                                                         byte[] payload, int payloadSize,
                                                         int transportId);

        [DllImport(Dll, CallingConvention = Cdecl)]
        internal static extern int mclx_handoff_control_encode(int operation, uint migrationRef,
                                                               uint sessionRef, byte[] challenge,
                                                               byte[] outBuf, int outCapacity);

        [DllImport(Dll, CallingConvention = Cdecl)]
        internal static extern int mclx_receive_handoff(IntPtr node, int arrivalTransport,
                                                        byte[] data, int size,
                                                        out int operation, out uint migrationRef,
                                                        out uint sessionRef, byte[] challenge,
                                                        out int challengeValid);

        [DllImport(Dll, CallingConvention = Cdecl)]
        internal static extern int mclx_apply_handoff(IntPtr node, int operation, uint migrationRef,
                                                      uint sessionRef, byte[] challenge,
                                                      int challengeValid, out int action);

        // ---- contact ----

        [DllImport(Dll, CallingConvention = Cdecl)]
        internal static extern int mclx_contact_snapshot(IntPtr node, out ContactSnapshot snapshot);

        [DllImport(Dll, CallingConvention = Cdecl)]
        internal static extern int mclx_contact_set_peer_ref(IntPtr node, uint peerRef);

        [DllImport(Dll, CallingConvention = Cdecl)]
        internal static extern int mclx_contact_record_offer(IntPtr node, uint migrationRef,
                                                             int transportId, int profileId,
                                                             uint endpointToken, int validity);

        [DllImport(Dll, CallingConvention = Cdecl)]
        internal static extern int mclx_contact_agree(IntPtr node, uint migrationRef, int transportId,
                                                      int profileId, uint sessionRef);

        [DllImport(Dll, CallingConvention = Cdecl)]
        internal static extern int mclx_contact_validation_begin(IntPtr node, byte[] challenge);

        [DllImport(Dll, CallingConvention = Cdecl)]
        internal static extern int mclx_contact_abandon_migration(IntPtr node);

        // ---- constants mirrored from the headers ----
        //
        // Only values that are part of the frame format or the state machine's
        // vocabulary. Anything a decision depends on is asked of the C above.

        internal const int ClassContact = 0;
        internal const int ClassData = 3;
        internal const int ClassAck = 4;
        internal const int ClassNack = 5;
        internal const int ClassHandoff = 8;

        internal const int FlagDestination = 0x01;
        internal const int FlagSession = 0x02;
        internal const int FlagSequence = 0x04;
        internal const int FlagFrameCheck = 0x10;

        internal const int KindPresence = 0;
        internal const int KindTransportOffer = 5;
        internal const int KindTransportAccept = 6;

        internal const int OpPathChallenge = 1;
        internal const int OpPathResponse = 2;
        internal const int OpCommit = 3;
        internal const int OpConfirm = 4;

        internal const int ActionNone = 0;
        internal const int ActionSendPathResponse = 1;
        internal const int ActionSendConfirm = 2;

        internal const int RoleInitiator = 0;
        internal const int RoleResponder = 1;

        internal const int LinkDiscovered = 1;
        internal const int LinkCapabilities = 2;
        internal const int LinkNegotiating = 3;
        internal const int LinkEstablished = 4;

        internal const int SdkOk = 0;
        internal const int SdkErrInvalidState = 8;
        internal const int SdkErrWrongTransport = 11;
        internal const int SdkErrQuiesced = 12;
        internal const int SdkNotAddressed = 13;

        internal static string StateName(int s) => s switch
        {
            0 => "NONE",
            1 => "ACTIVE",
            2 => "OFFERED",
            3 => "AGREED",
            4 => "VALIDATING",
            5 => "VALIDATED",
            6 => "COMMITTING",
            7 => "CLOSED",
            _ => "?" + s
        };

        internal static string TransportName(int t) => t switch
        {
            0 => "none",
            1 => "AP",
            2 => "IP",
            3 => "BLE",
            4 => "UWB",
            _ => "?" + t
        };

        internal static string SdkStatusName(int s) => s switch
        {
            0 => "OK",
            1 => "INVALID_ARGUMENT",
            2 => "BUFFER_TOO_SMALL",
            3 => "WIRE_FAILURE",
            4 => "LINK_FAILURE",
            5 => "TX_UNAVAILABLE",
            6 => "TX_FAILURE",
            7 => "FRAME_FAILURE",
            8 => "INVALID_STATE",
            9 => "TX_NOT_SENT",
            10 => "TX_UNCERTAIN",
            11 => "WRONG_TRANSPORT",
            12 => "QUIESCED",
            13 => "NOT_ADDRESSED",
            -1000 => "SHIM_ARGUMENT",
            _ => "status " + s
        };
    }
}
