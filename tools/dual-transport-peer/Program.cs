// MCL dual-transport migration harness.
//
// One contact, two live radios, and a real migration between them.
//
// Every over-air run before this one exercised a single binding against a host
// over a single medium. That establishes carriage. It cannot establish
// migration, which is the property that a contact SURVIVES a change of medium --
// and a peer with one radio has no medium to change to.
//
// This program holds a Bluetooth LE connection and a UDP socket to the same
// board at the same time, and drives one mcl_node_t across both. It supplies
// radios, timing and assertions. Every frame it sends and every decision it
// makes about one it receives belongs to mcl_dual_host.dll, compiled from the
// same sources the firmware runs.
//
// WHAT THIS MEASURES
//
//   - the four handoff controls crossing real radios, decoded by the peer's own
//     codec rather than by a test harness;
//   - a control arriving on the WRONG transport being refused, which is the one
//     check path validation depends on and which a single-transport rig cannot
//     perform at all;
//   - contact continuity: same session reference, same peer reference, after
//     the medium underneath changes;
//   - repair by retransmission when a COMMIT or a CONFIRM is lost.
//
// WHAT IT DOES NOT MEASURE
//
//   - independent interoperability. Both ends compile the same sources, so a
//     shared misreading of the specification passes on both sides and is
//     invisible here. C4 and E6 need a second implementation written by someone
//     else from the specification.
//   - security. There is none. Every reference in the exchange crosses both
//     media in the clear, and a listener in range can quote all of them back.

using System;
using System.Collections.Generic;
using System.Linq;
using System.Net;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Threading;
using System.Threading.Tasks;

namespace Mcl.Sdk.DualTransport
{
    internal static class Program
    {
        // Integrator-scoped identifiers, matching the firmware.
        private const string ServiceUuid = "6d636c00-0001-4d43-4c00-6d636c626c65";
        private const string RxCharUuid = "6d636c00-0002-4d43-4c00-6d636c626c65";
        private const string TxCharUuid = "6d636c00-0003-4d43-4c00-6d636c626c65";

        private const uint HostSourceRef = 0xC0FFEE05u;

        // Transport-scoped profile identifiers, from each binding's registry.
        // Profile 192 under IP and profile 192 under BLE are unrelated
        // assignments and must never be compared.
        private const int IpProfileDatagram = 192;   // IP-X0-DATAGRAM-EXPERIMENTAL
        private const int BleProfileGatt = 192;      // BLE-X0-GATT-EXPERIMENTAL

        private static IntPtr _node;
        private static TxFn _txDelegate;             // kept alive against the GC
        private static readonly InboundQueue Queue = new();
        private static BleRadio _ble;
        private static IpRadio _ip;
        private static Board _board;

        private static int _bleTransport;
        private static int _ipTransport;

        private static bool _dropNextTx;
        private static int _hostDroppedFrames;
        private static byte[] _lastTxFrame;

        private static int _checks;
        private static int _failed;
        private static Dictionary<string, string> _finalBoardStatus;

        // ------------------------------------------------------------ checks

        private static void Check(bool condition, string message)
        {
            _checks++;
            if (condition) { return; }
            _failed++;
            Console.WriteLine("    FAIL: " + message);
            _board?.Note("FAIL: " + message);
        }

        private static void Step(string label)
        {
            Console.WriteLine("  [+] " + label);
            _board?.Note("step: " + label);
        }

        // ------------------------------------------------------------ transmit

        /*
         * The one transmit callback. The SDK says which bearer; this dispatches.
         *
         * The three-way return is passed through from the radio unchanged. A
         * radio that claimed certainty it did not have would either strand the
         * contact in COMMITTING over a frame nobody saw, or roll back a commit
         * the peer may already have acted on.
         */
        private static int Tx(IntPtr user, byte transportId, IntPtr data, UIntPtr size)
        {
            var frame = new byte[(int)size];
            Marshal.Copy(data, frame, 0, frame.Length);
            _lastTxFrame = frame;

            if (_dropNextTx)
            {
                /*
                 * Report ACCEPTED and then discard. That is the case that
                 * matters: a transmit failure the sender can see is easy to
                 * handle, and a frame that vanishes after a successful send is
                 * what actually happens on a radio.
                 */
                _dropNextTx = false;
                _hostDroppedFrames++;
                _board?.Note($"host dropped a frame on purpose: transport=" +
                             $"{Mcl.TransportName(transportId)} n={frame.Length}");
                return 0;
            }

            if (transportId == _bleTransport) { return _ble.Send(frame); }
            if (transportId == _ipTransport) { return _ip.Send(frame); }
            return -1;   // a transport this host does not have: definitely not sent
        }

        // ------------------------------------------------------------ receive

        private sealed class Received
        {
            public int Transport;
            public byte[] Frame;
            public bool IsHandoff;
            public int Status;
            public Rx Rx;
            public int Op;
            public uint MigrationRef;
            public uint SessionRef;
            public byte[] Challenge;
            public int ChallengeValid;
        }

        /*
         * Take the next frame from either radio and decode it.
         *
         * The frame class is peeked from the first byte -- link_major in the high
         * nibble, frame_class in the low -- because a handoff control and a
         * semantic object are different payload contracts, and the class is what
         * says which registry the payload's first bytes belong to. Nothing else
         * is interpreted here.
         */
        private static Received Receive(int timeoutMs)
        {
            var inbound = Queue.Take(timeoutMs);
            if (inbound == null) { return null; }

            var r = new Received { Transport = inbound.Transport, Frame = inbound.Frame };

            if (inbound.Frame.Length >= 1 && (inbound.Frame[0] & 0x0F) == Mcl.ClassHandoff)
            {
                r.IsHandoff = true;
                r.Challenge = new byte[Mcl.mclx_challenge_size()];
                r.Status = Mcl.mclx_receive_handoff(_node, inbound.Transport,
                                                    inbound.Frame, inbound.Frame.Length,
                                                    out r.Op, out r.MigrationRef,
                                                    out r.SessionRef, r.Challenge,
                                                    out r.ChallengeValid);
            }
            else
            {
                r.Status = Mcl.mclx_receive_framed(_node, inbound.Transport,
                                                   inbound.Frame, inbound.Frame.Length,
                                                   out r.Rx);
            }
            return r;
        }

        private static ContactSnapshot Snapshot()
        {
            Mcl.mclx_contact_snapshot(_node, out var snapshot);
            return snapshot;
        }

        private static void ReportContact(string label)
        {
            var c = Snapshot();
            string line = $"{label}: state={Mcl.StateName(c.State)} " +
                          $"active={Mcl.TransportName(c.ActiveTransport)} " +
                          $"ctrl={Mcl.TransportName(c.ControlTransport)} " +
                          $"quiesced={c.Quiesced} session={c.SessionRef:X8} " +
                          $"peer={c.PeerRef:X8} hops={c.MigrationCount} " +
                          $"link={c.LinkState}";
            Console.WriteLine("      " + line);
            _board?.Note(line);
        }

        // ------------------------------------------------------------ main

        private static async Task<int> Main(string[] args)
        {
            string bind = null, peer = "192.168.4.1", serial = "COM3", logPath = "board-serial.log";
            int port = 5555, cycles = 100;

            for (int i = 0; i < args.Length; i++)
            {
                switch (args[i])
                {
                    case "--bind" when i + 1 < args.Length: bind = args[++i]; break;
                    case "--peer" when i + 1 < args.Length: peer = args[++i]; break;
                    case "--port" when i + 1 < args.Length: port = int.Parse(args[++i]); break;
                    case "--serial" when i + 1 < args.Length: serial = args[++i]; break;
                    case "--cycles" when i + 1 < args.Length: cycles = int.Parse(args[++i]); break;
                    case "--log" when i + 1 < args.Length: logPath = args[++i]; break;
                }
            }

            if (bind == null)
            {
                Console.WriteLine("--bind <local ip on the board's access point> is required.");
                Console.WriteLine("Without it the routing table chooses the interface, and the");
                Console.WriteLine("result then describes a different link from the one measured.");
                return 2;
            }

            Console.WriteLine("MCL dual-transport migration harness");
            Console.WriteLine("====================================");

            _bleTransport = Mcl.mclx_transport_ble();
            _ipTransport = Mcl.mclx_transport_ip();
            Console.WriteLine($"frame_max={Mcl.mclx_frame_max_size()} " +
                              $"ble_mtu={Mcl.mclx_ble_default_mtu()} " +
                              $"challenge={Mcl.mclx_challenge_size()} " +
                              $"transports: BLE={_bleTransport} IP={_ipTransport}");
            Console.WriteLine($"host source_ref={HostSourceRef:X8} " +
                              $"udp {bind} -> {peer}:{port} serial={serial}");
            Console.WriteLine();

            try
            {
                _board = new Board(serial, logPath);
            }
            catch (Exception ex)
            {
                Console.WriteLine($"Could not open {serial}: {ex.Message}");
                return 2;
            }

            _board.Note("harness start");
            _board.Send("RESET");
            if (_board.Await("MCLDUAL RESET OK", 4000) == null)
            {
                Console.WriteLine("Board did not answer RESET. Is the dual-peer firmware running?");
                _board.Dispose();
                return 2;
            }
            Console.WriteLine("  board reset to a fresh contact on BLE");

            _ip = new IpRadio(Queue, IPAddress.Parse(bind),
                              new IPEndPoint(IPAddress.Parse(peer), port));
            Console.WriteLine($"  udp socket bound to {_ip.LocalEndPoint}");

            _ble = new BleRadio(Queue, ServiceUuid, RxCharUuid, TxCharUuid);
            ulong address = await _ble.ScanAsync(25);
            if (address == 0)
            {
                Console.WriteLine("  board advertisement not seen; is BLE up?");
                Cleanup();
                return 2;
            }
            Console.WriteLine($"  board advertising at {address:X12}");

            if (!await _ble.ConnectAsync(address))
            {
                Console.WriteLine("  could not open the GATT session");
                Cleanup();
                return 2;
            }
            Console.WriteLine("  GATT session open, notifications subscribed");
            Console.WriteLine();

            _node = Marshal.AllocHGlobal(Mcl.mclx_node_size());
            _txDelegate = Tx;
            Check(Mcl.mclx_node_init(_node, HostSourceRef, _bleTransport,
                                     Mcl.RoleInitiator, _txDelegate, IntPtr.Zero) == Mcl.SdkOk,
                  "node initialised on BLE as initiator");

            try
            {
                if (FirstContact())
                {
                    WrongTransportBeforeAnyMigration();
                    OfferRefusals();
                    Migrate("BLE -> IP", _ipTransport, IpProfileDatagram, 0x1001u,
                            new Options { WrongTransportProbe = true,
                                          DuplicateOfferAfterAccept = true,
                                          DuplicateChallenge = true });
                    TrafficOnTheNewPath();
                    Migrate("IP -> BLE", _bleTransport, BleProfileGatt, 0x1002u, new Options());
                    HandoffLevelRefusals();
                    Migrate("BLE -> IP, host's COMMIT lost", _ipTransport, IpProfileDatagram, 0x1003u,
                            new Options { DropOwnCommitOnce = true });
                    Migrate("IP -> BLE, board's CONFIRM lost", _bleTransport, BleProfileGatt, 0x1004u,
                            new Options { DropBoardConfirmOnce = true });
                    RepeatedCycles(cycles);
                    AbandonedMigrationLeavesTheOldPath();
                }
            }
            catch (Exception ex)
            {
                Check(false, $"harness threw: {ex}");
            }

            Summary();
            Cleanup();
            return _failed == 0 ? 0 : 1;
        }

        private static void Cleanup()
        {
            if (_node != IntPtr.Zero) { Marshal.FreeHGlobal(_node); _node = IntPtr.Zero; }
            _ble?.Dispose();
            _ip?.Dispose();
            _board?.Dispose();
        }

        // ------------------------------------------------------------ phase A

        /*
         * First contact on BLE, and the link lifecycle walked on real events.
         *
         * Nothing jumps to ESTABLISHED. A migration may only be driven from
         * ESTABLISHED or HANDOFF, and it would be easy to satisfy that by
         * transitioning through the whole lifecycle at startup -- which would be
         * a lie, because nothing would have been discovered or negotiated.
         */
        private static bool FirstContact()
        {
            Step("first contact on BLE");

            Check(Mcl.mclx_send_presence(_node, Mcl.ClassContact,
                                         Mcl.FlagSequence | Mcl.FlagFrameCheck, 0) == Mcl.SdkOk,
                  "unaddressed PRESENCE sent on BLE");

            var reply = Receive(8000);
            Check(reply != null, "board answered on BLE");
            if (reply == null) { return false; }

            Check(reply.Transport == _bleTransport, "the answer arrived on BLE");
            Check(reply.Status == Mcl.SdkOk,
                  $"the answer decodes ({Mcl.SdkStatusName(reply.Status)})");
            if (reply.Status != Mcl.SdkOk) { return false; }
            Check(reply.Rx.Kind == Mcl.KindPresence, "the answer carries PRESENCE");

            Check(Mcl.mclx_link_transition(_node, Mcl.LinkDiscovered) == Mcl.SdkOk,
                  "link DISCOVERED on a peer actually heard from");
            Check(Mcl.mclx_contact_set_peer_ref(_node, reply.Rx.SourceRef) == 0,
                  "peer contact reference recorded");
            Console.WriteLine($"      board source_ref={reply.Rx.SourceRef:X8}");

            /*
             * An ADDRESSED frame. Before the addressing fix this could not be
             * expressed at all: the destination flag was honoured and the
             * reference left at zero, so the frame was addressed to nobody and
             * the board refused it as NOT_ADDRESSED.
             */
            Step("an addressed frame reaches the peer it names");
            Check(Mcl.mclx_send_presence(_node, Mcl.ClassData,
                                         Mcl.FlagSequence | Mcl.FlagFrameCheck, 1) == Mcl.SdkOk,
                  "addressed PRESENCE sent on BLE");
            var addressed = Receive(8000);
            Check(addressed != null && addressed.Status == Mcl.SdkOk,
                  "board accepted and answered an addressed frame");
            if (addressed != null && addressed.Status == Mcl.SdkOk)
            {
                Check(addressed.Rx.DestinationRef == HostSourceRef,
                      "the board's answer names this host as its destination");
            }

            // The offer/acceptance exchange that follows IS the negotiation.
            Check(Mcl.mclx_link_transition(_node, Mcl.LinkCapabilities) == Mcl.SdkOk,
                  "link CAPABILITIES");
            Check(Mcl.mclx_link_transition(_node, Mcl.LinkNegotiating) == Mcl.SdkOk,
                  "link NEGOTIATING");
            Check(Mcl.mclx_link_transition(_node, Mcl.LinkEstablished) == Mcl.SdkOk,
                  "link ESTABLISHED");

            ReportContact("contact");
            return true;
        }

        // ------------------------------------------------------------ phase B

        /*
         * A frame that is correct in every field, replayed on a medium this
         * contact does not live on.
         *
         * THIS IS THE CHECK A SINGLE-TRANSPORT RIG CANNOT MAKE. Accepting it
         * would let any bearer a process happens to have open inject frames into
         * a contact established somewhere else. The bytes here are literally the
         * ones the board already accepted over BLE, so nothing about the frame
         * itself can be the reason it is refused.
         */
        private static void WrongTransportBeforeAnyMigration()
        {
            Step("a valid frame replayed on a transport the contact does not use");

            var before = _board.Status();
            long wrongBefore = Board.Counter(before, "wrongxp");
            long rxIpBefore = Board.Counter(before, "rx_ip");

            Check(_lastTxFrame != null, "a previously accepted frame is available to replay");
            if (_lastTxFrame == null) { return; }

            Check(_ip.Send(_lastTxFrame) == 0, "the same bytes were sent over UDP");

            var stray = Receive(3000);
            Check(stray == null, "the board did not answer a frame on the wrong transport");

            var after = _board.Status();
            long wrongAfter = Board.Counter(after, "wrongxp");
            long rxIpAfter = Board.Counter(after, "rx_ip");

            Check(rxIpAfter > rxIpBefore, "the board's radio did receive the datagram");
            Check(wrongAfter == wrongBefore + 1,
                  $"the board refused it as WRONG_TRANSPORT (counter {wrongBefore} -> {wrongAfter})");
            Console.WriteLine($"      board rx_ip {rxIpBefore}->{rxIpAfter}, " +
                              $"wrongxp {wrongBefore}->{wrongAfter}");
        }

        // ------------------------------------------------------------ phase B2

        /*
         * Offers that must be refused before any of them reaches a radio.
         *
         * These are refused by this end, which is the point: an offer that is
         * malformed by the registries' own rules should never be put on the air
         * for a peer to have to reason about. Each is asserted to leave the
         * contact exactly where it was.
         */
        private static void OfferRefusals()
        {
            Step("offers that must be refused");

            var before = Snapshot();

            // Profile zero is reserved in every transport's profile registry so
            // that an uninitialised field names no profile. All four registries
            // said so and nothing enforced it until this run.
            Check(Mcl.mclx_contact_record_offer(_node, 0x0A01u, _ipTransport,
                                                0, 0xE9000A01u, 30) == 1 /* INVALID_ARGUMENT */,
                  "an offer naming the reserved profile 0 is refused");

            // Changing profile on the transport already in use is adaptation,
            // not migration. Accepting it would let a peer complete a
            // "migration" without demonstrating reachability anywhere else.
            Check(Mcl.mclx_contact_record_offer(_node, 0x0A02u, before.ActiveTransport,
                                                BleProfileGatt, 0xE9000A02u, 30) == 1,
                  "an offer naming the transport already in use is refused");

            // Migration reference zero names no transaction.
            Check(Mcl.mclx_contact_record_offer(_node, 0u, _ipTransport,
                                                IpProfileDatagram, 0xE9000A03u, 30) == 1,
                  "an offer with no migration reference is refused");

            var after = Snapshot();
            Check(after.State == before.State &&
                  after.PendingMigrationRef == before.PendingMigrationRef,
                  "none of the refusals left a pending transaction behind");
        }

        // ------------------------------------------------------------ migration

        private sealed class Options
        {
            /* Deliver a valid PATH_CHALLENGE on the OLD path while the candidate
             * is the one being validated. It must be refused. */
            public bool WrongTransportProbe;
            /* Drop this host's own COMMIT once, then retransmit it. */
            public bool DropOwnCommitOnce;
            /* Have the board drop its CONFIRM once, then repair by retransmitting
             * the COMMIT. */
            public bool DropBoardConfirmOnce;
            /* Re-send the offer after the board has already accepted it. */
            public bool DuplicateOfferAfterAccept;
            /* Re-send the same PATH_CHALLENGE after it has been answered. */
            public bool DuplicateChallenge;
            public bool Quiet;
        }

        private static readonly byte[] ChallengeBuffer = new byte[8];

        private static byte[] NewChallenge()
        {
            var challenge = new byte[Mcl.mclx_challenge_size()];
            RandomNumberGenerator.Fill(challenge);
            return challenge;
        }

        /*
         * Drive one complete migration, as the controlling peer.
         *
         *   TRANSPORT_OFFER / ACCEPT        old transport
         *   PATH_CHALLENGE / PATH_RESPONSE  candidate transport
         *   COMMIT / CONFIRM                candidate transport
         *
         * The host never tells the SDK which bearer to use. The SDK derives it
         * from the contact state and tells the transmit callback, which is what
         * makes "this control must go out on the candidate" a property of the
         * library rather than of this file.
         */
        private static bool Migrate(string label, int candidate, int profile,
                                    uint migrationRef, Options options)
        {
            if (!options.Quiet) { Step($"migration: {label}"); }

            var before = Snapshot();
            uint sessionBefore = before.SessionRef;
            uint peerBefore = before.PeerRef;
            int hopsBefore = before.MigrationCount;

            // ---- offer ----
            uint endpointToken = 0xE9000000u | migrationRef;
            int status = Mcl.mclx_contact_record_offer(_node, migrationRef, candidate,
                                                       profile, endpointToken, 30);
            Check(status == 0, $"offer recorded locally ({status})");
            if (status != 0) { return false; }

            int flags = Mcl.FlagSequence | Mcl.FlagFrameCheck;
            if (before.SessionValid != 0) { flags |= Mcl.FlagSession; }
            status = Mcl.mclx_send_transport_offer(_node, flags, 1, migrationRef,
                                                   candidate, profile, endpointToken, 30);
            Check(status == Mcl.SdkOk, $"TRANSPORT_OFFER sent ({Mcl.SdkStatusName(status)})");
            if (status != Mcl.SdkOk) { return false; }

            // ---- acceptance ----
            var accept = AwaitObject(Mcl.KindTransportAccept, 8000);
            Check(accept != null, "board answered with TRANSPORT_ACCEPT");
            if (accept == null) { return false; }

            Check(accept.Rx.MigrationRef == migrationRef,
                  "the acceptance names this migration");
            Check(accept.Rx.TransportId == candidate,
                  "the acceptance names the offered transport");
            Check(accept.Rx.ProfileId == profile, "the acceptance names the offered profile");

            status = Mcl.mclx_contact_agree(_node, migrationRef, candidate, profile,
                                            accept.Rx.SessionRef);
            Check(status == 0, $"acceptance applied ({status})");
            if (status != 0) { return false; }

            if (sessionBefore != 0)
            {
                Check(accept.Rx.SessionRef == sessionBefore,
                      "the session reference is the one bound by the first acceptance");
            }

            // ---- the wrong-transport probe ----
            if (options.WrongTransportProbe) { WrongTransportHandoffProbe(migrationRef, accept.Rx.SessionRef); }

            /*
             * A duplicate of the offer, arriving after it was already accepted.
             *
             * On a medium that reorders, a delayed copy of a step already
             * completed is ordinary. It must not return the contact to OFFERED
             * and undo the acceptance made after it.
             */
            if (options.DuplicateOfferAfterAccept)
            {
                Console.WriteLine("  [-] a duplicate TRANSPORT_OFFER after the acceptance");
                long rejBefore = Board.Counter(_board.Status(), "rej");
                Check(Mcl.mclx_send_transport_offer(_node, flags, 1, migrationRef, candidate,
                                                    profile, endpointToken, 30) == Mcl.SdkOk,
                      "the duplicate offer was delivered");
                var echo = AwaitObject(Mcl.KindTransportAccept, 2500);
                Check(echo == null, "the board did not re-accept a migration it had already accepted");
                _board.Status();   // one more line into the log
                Check(Snapshot().State == 3 /* AGREED */,
                      "this end is still AGREED, not returned to OFFERED");
            }

            // ---- path validation on the candidate ----
            var challenge = NewChallenge();
            status = Mcl.mclx_contact_validation_begin(_node, challenge);
            Check(status == 0, $"validation begun ({status})");
            if (status != 0) { return false; }

            status = Mcl.mclx_send_handoff(_node, Mcl.OpPathChallenge, migrationRef,
                                           accept.Rx.SessionRef, challenge,
                                           Mcl.FlagSession | Mcl.FlagSequence | Mcl.FlagFrameCheck);
            Check(status == Mcl.SdkOk, $"PATH_CHALLENGE sent ({Mcl.SdkStatusName(status)})");
            if (status != Mcl.SdkOk) { return false; }

            var response = AwaitHandoff(Mcl.OpPathResponse, 8000);
            Check(response != null, "board answered PATH_RESPONSE on the candidate");
            if (response == null) { return false; }
            Check(response.Transport == candidate,
                  "the response arrived on the candidate transport, not the old one");
            Check(response.Challenge.SequenceEqual(challenge),
                  "the response echoes the challenge exactly");

            status = Mcl.mclx_apply_handoff(_node, response.Op, response.MigrationRef,
                                            response.SessionRef, response.Challenge,
                                            response.ChallengeValid, out int action);
            Check(status == Mcl.SdkOk, $"PATH_RESPONSE applied ({Mcl.SdkStatusName(status)})");
            Check(action == Mcl.ActionNone, "a response calls for nothing to be sent back");

            /*
             * The duplicate PATH_CHALLENGE row, over a real radio.
             *
             * This is the case a radio would have found and a loopback test
             * never does: B echoes a challenge and reaches VALIDATED, its
             * response is lost, A correctly retransmits -- and a receiver that
             * accepts a challenge only in AGREED refuses the duplicate. One
             * dropped frame would kill the migration with both peers behaving
             * correctly. The board must re-echo the SAME challenge and move
             * nothing.
             */
            if (options.DuplicateChallenge)
            {
                Console.WriteLine("  [-] the same PATH_CHALLENGE sent a second time");
                status = Mcl.mclx_send_handoff(_node, Mcl.OpPathChallenge, migrationRef,
                                               accept.Rx.SessionRef, challenge,
                                               Mcl.FlagSession | Mcl.FlagSequence | Mcl.FlagFrameCheck);
                Check(status == Mcl.SdkOk, "the retransmitted challenge was sent");
                var again = AwaitHandoff(Mcl.OpPathResponse, 6000);
                Check(again != null, "the board answered the retransmission");
                if (again != null)
                {
                    Check(again.Challenge.SequenceEqual(challenge),
                          "it re-echoed the same challenge rather than refusing");
                    int dup = Mcl.mclx_apply_handoff(_node, again.Op, again.MigrationRef,
                                                     again.SessionRef, again.Challenge,
                                                     again.ChallengeValid, out int dupAction);
                    Check(dup == Mcl.SdkOk, $"the duplicate response applies ({Mcl.SdkStatusName(dup)})");
                    Check(Snapshot().State == 5 /* VALIDATED */,
                          "the contact is still VALIDATED, not moved by a duplicate");
                }
            }

            // ---- commit ----
            if (options.DropBoardConfirmOnce)
            {
                _board.Send("DROPNEXT");
                Check(_board.Await("MCLDUAL DROPNEXT ARMED", 3000) != null,
                      "board armed to discard its next transmission");
            }
            if (options.DropOwnCommitOnce) { _dropNextTx = true; }

            status = Mcl.mclx_send_handoff(_node, Mcl.OpCommit, migrationRef,
                                           accept.Rx.SessionRef, null,
                                           Mcl.FlagSession | Mcl.FlagSequence | Mcl.FlagFrameCheck);
            Check(status == Mcl.SdkOk, $"COMMIT sent ({Mcl.SdkStatusName(status)})");

            var committing = Snapshot();
            Check(committing.State == 6 /* COMMITTING */,
                  $"contact is COMMITTING after the commit ({Mcl.StateName(committing.State)})");
            Check(committing.Quiesced == 1,
                  "ordinary traffic is quiesced while the commit is outstanding");
            Check(Mcl.mclx_send_presence(_node, Mcl.ClassData, Mcl.FlagSequence, 1)
                  == Mcl.SdkErrQuiesced,
                  "the SDK refuses ordinary traffic in that window");

            var confirm = AwaitHandoff(Mcl.OpConfirm, options.DropOwnCommitOnce ||
                                                      options.DropBoardConfirmOnce ? 3000 : 8000);

            if (confirm == null && (options.DropOwnCommitOnce || options.DropBoardConfirmOnce))
            {
                /*
                 * The repair. One dropped frame must not split the peers: the
                 * only remedy on a lossy medium is retransmission, and every
                 * control is answerable a second time with the same outcome.
                 *
                 * A retransmitted COMMIT covers both losses. If the COMMIT was
                 * lost the board has not yet migrated and acts on it now; if the
                 * CONFIRM was lost the board is already ACTIVE and re-confirms.
                 * The sender cannot tell which happened, and does not need to.
                 */
                Console.WriteLine("      no CONFIRM: retransmitting the COMMIT");
                _board.Note("retransmitting COMMIT after a deliberate loss");
                status = Mcl.mclx_send_handoff(_node, Mcl.OpCommit, migrationRef,
                                               accept.Rx.SessionRef, null,
                                               Mcl.FlagSession | Mcl.FlagSequence | Mcl.FlagFrameCheck);
                Check(status == Mcl.SdkOk,
                      $"COMMIT retransmitted ({Mcl.SdkStatusName(status)})");
                confirm = AwaitHandoff(Mcl.OpConfirm, 8000);
                Check(confirm != null, "the retransmission was answered with CONFIRM");
            }

            Check(confirm != null, "board answered CONFIRM");
            if (confirm == null) { return false; }
            Check(confirm.Transport == candidate, "the confirmation arrived on the candidate");

            status = Mcl.mclx_apply_handoff(_node, confirm.Op, confirm.MigrationRef,
                                            confirm.SessionRef, confirm.Challenge,
                                            confirm.ChallengeValid, out action);
            Check(status == Mcl.SdkOk, $"CONFIRM applied ({Mcl.SdkStatusName(status)})");

            // ---- continuity ----
            var after = Snapshot();
            Check(after.State == 1 /* ACTIVE */, $"contact is ACTIVE ({Mcl.StateName(after.State)})");
            Check(after.ActiveTransport == candidate,
                  $"the contact is now carried on {Mcl.TransportName(candidate)}");
            Check(after.MigrationCount == hopsBefore + 1, "the migration was counted once");
            Check(after.PeerRef == peerBefore || peerBefore == 0,
                  "the peer reference is unchanged across the medium change");
            if (sessionBefore != 0)
            {
                Check(after.SessionRef == sessionBefore,
                      "the session reference is unchanged across the medium change");
            }
            Check(after.CompletedMigrationRef == migrationRef,
                  "the completed migration is the one that was driven");

            if (!options.Quiet) { ReportContact("after"); }
            return true;
        }

        /*
         * A control that is correct in every reference, delivered over the medium
         * the contact is LEAVING while the candidate is the one under test.
         *
         * A single-transport rig cannot construct this case, and no field in the
         * control can be the reason it is refused. Accepting it would validate a
         * candidate path on the strength of bytes that never crossed it, which is
         * precisely, and only, what PATH_CHALLENGE exists to establish.
         */
        private static void WrongTransportHandoffProbe(uint migrationRef, uint sessionRef)
        {
            Console.WriteLine("  [-] a valid PATH_CHALLENGE delivered on the old path");

            var before = _board.Status();
            long wrongBefore = Board.Counter(before, "wrongxp");

            var challenge = NewChallenge();
            var payload = new byte[64];
            int n = Mcl.mclx_handoff_control_encode(Mcl.OpPathChallenge, migrationRef,
                                                    sessionRef, challenge, payload, payload.Length);
            Check(n > 0, $"a valid control was encoded ({n})");
            if (n <= 0) { return; }

            int status = Mcl.mclx_send_handoff_raw(_node, sessionRef, payload.Take(n).ToArray(),
                                                   n, _bleTransport);
            Check(status == Mcl.SdkOk, "the control was delivered on the old transport");

            var stray = Receive(3000);
            Check(stray == null, "the board sent no PATH_RESPONSE for it");

            long wrongAfter = Board.Counter(_board.Status(), "wrongxp");
            Check(wrongAfter == wrongBefore + 1,
                  $"the board refused it as WRONG_TRANSPORT ({wrongBefore} -> {wrongAfter})");
        }

        // ------------------------------------------------------------ phase D

        private static void TrafficOnTheNewPath()
        {
            Step("ordinary traffic on the transport just migrated to");

            for (int i = 0; i < 3; i++)
            {
                int status = Mcl.mclx_send_presence(_node, Mcl.ClassData,
                                                    Mcl.FlagSequence | Mcl.FlagFrameCheck |
                                                    Mcl.FlagSession, 1);
                Check(status == Mcl.SdkOk, $"PRESENCE {i} sent ({Mcl.SdkStatusName(status)})");
                var reply = Receive(6000);
                Check(reply != null && reply.Status == Mcl.SdkOk, $"board answered {i}");
                if (reply != null)
                {
                    Check(reply.Transport == _ipTransport,
                          "the answer came back on the migrated-to transport");
                }
            }
        }

        // ------------------------------------------------------------ phase F

        /*
         * Handoff controls that must be refused, delivered over a real radio.
         *
         * Every case here is a no-op by design: a control that does not match the
         * contact's state or its transaction references is refused WITHOUT
         * changing anything, and the old working transport is left intact. A
         * failed migration must never destroy the contact, so after each case the
         * contact is asserted to be exactly where it was.
         */
        private static void HandoffLevelRefusals()
        {
            Step("handoff controls that must be refused");

            var start = Snapshot();
            uint session = start.SessionRef;
            long rejectedBefore = Board.Counter(_board.Status(), "rej");

            var valid = new byte[64];
            int validLength = Mcl.mclx_handoff_control_encode(Mcl.OpConfirm, 0x7777u,
                                                              session, null, valid, valid.Length);
            Check(validLength > 0, "a well-formed CONFIRM encodes");
            if (validLength <= 0) { return; }

            // 1. A control whose transaction nothing matches. It decodes; applying
            //    it must fail, and the contact must not move.
            Console.WriteLine("  [-] a CONFIRM naming a migration that does not exist");
            Check(Mcl.mclx_send_handoff_raw(_node, session, valid.Take(validLength).ToArray(),
                                            validLength, start.ActiveTransport) == Mcl.SdkOk,
                  "delivered");
            Check(Receive(2500) == null, "the board did not act on it");

            // 2. A truncated payload. The control decoder must refuse it before
            //    anything reads a field out of it.
            Console.WriteLine("  [-] a truncated handoff control payload");
            Check(Mcl.mclx_send_handoff_raw(_node, session,
                                            valid.Take(validLength - 1).ToArray(),
                                            validLength - 1, start.ActiveTransport) == Mcl.SdkOk,
                  "delivered");
            Check(Receive(2500) == null, "the board did not act on it");

            // 3. An operation nobody has assigned. The operation byte is located
            //    by comparing two encodings rather than hardcoded here, so this
            //    case cannot quietly stop testing what it claims to if the layout
            //    ever changes.
            var other = new byte[64];
            int otherLength = Mcl.mclx_handoff_control_encode(Mcl.OpCommit, 0x7777u,
                                                              session, null, other, other.Length);
            int opOffset = -1;
            if (otherLength == validLength)
            {
                for (int i = 0; i < validLength; i++)
                {
                    if (valid[i] != other[i]) { opOffset = opOffset < 0 ? i : -2; }
                }
            }
            Check(opOffset >= 0, "the operation byte was located by differencing two encodings");
            if (opOffset >= 0)
            {
                Console.WriteLine("  [-] an unassigned handoff operation");
                var unassigned = valid.Take(validLength).ToArray();
                unassigned[opOffset] = 0x7F;   // assigned to nothing
                Check(Mcl.mclx_send_handoff_raw(_node, session, unassigned, validLength,
                                                start.ActiveTransport) == Mcl.SdkOk, "delivered");
                Check(Receive(2500) == null, "the board did not act on it");
            }

            var end = Snapshot();
            Check(end.State == start.State && end.ActiveTransport == start.ActiveTransport &&
                  end.SessionRef == start.SessionRef &&
                  end.MigrationCount == start.MigrationCount,
                  "none of the refusals disturbed the contact");

            long rejectedAfter = Board.Counter(_board.Status(), "rej");
            Check(rejectedAfter > rejectedBefore,
                  $"the board counted the refusals ({rejectedBefore} -> {rejectedAfter})");

            Step("the contact still carries traffic after the refusals");
            Check(Mcl.mclx_send_presence(_node, Mcl.ClassData,
                                         Mcl.FlagSequence | Mcl.FlagFrameCheck | Mcl.FlagSession, 1)
                  == Mcl.SdkOk, "PRESENCE sent");
            var reply = Receive(6000);
            Check(reply != null && reply.Status == Mcl.SdkOk, "board answered");
        }

        // ------------------------------------------------------------ phase H

        /*
         * Migration is not a thing that has to work once. Alternating BLE and IP
         * for the whole run exercises the transaction references, the session
         * binding and both radios' coexistence on one 2.4 GHz front end.
         */
        private static void RepeatedCycles(int cycles)
        {
            if (cycles <= 0) { return; }
            Step($"{cycles} consecutive migrations, alternating BLE and IP");

            var start = Snapshot();
            uint session = start.SessionRef;
            int completed = 0;
            int startingHops = start.MigrationCount;
            var began = DateTime.UtcNow;

            for (int i = 0; i < cycles; i++)
            {
                var current = Snapshot();
                bool onBle = current.ActiveTransport == _bleTransport;
                int candidate = onBle ? _ipTransport : _bleTransport;
                int profile = onBle ? IpProfileDatagram : BleProfileGatt;

                if (!Migrate($"cycle {i + 1}", candidate, profile, 0x2000u + (uint)i,
                             new Options { Quiet = true }))
                {
                    Console.WriteLine($"      cycle {i + 1} did not complete");
                    break;
                }
                completed++;

                if ((i + 1) % 10 == 0)
                {
                    Console.WriteLine($"      {i + 1} migrations, " +
                                      $"now on {Mcl.TransportName(Snapshot().ActiveTransport)}");
                }
            }

            var elapsed = DateTime.UtcNow - began;
            var end = Snapshot();
            Console.WriteLine($"      completed={completed} of {cycles} in {elapsed.TotalSeconds:F1}s");
            Check(completed == cycles, $"every cycle completed ({completed} of {cycles})");
            Check(end.MigrationCount == startingHops + completed,
                  "the contact counted exactly the migrations that happened");
            Check(end.SessionRef == session,
                  "one session reference survived every hop");
        }

        // ------------------------------------------------------------ phase I

        /*
         * A migration that is offered, accepted, and then goes no further.
         *
         * The candidate never carries a byte, so the contact must stay exactly
         * where it is: a failed migration must never destroy a working contact.
         * Both peers are returned to a clean state afterwards, because a board
         * left holding a pending transaction would make every later run start
         * from somewhere undocumented.
         */
        private static void AbandonedMigrationLeavesTheOldPath()
        {
            Step("a migration abandoned after acceptance leaves the old path working");

            var before = Snapshot();
            int oldTransport = before.ActiveTransport;
            bool onBle = oldTransport == _bleTransport;
            int candidate = onBle ? _ipTransport : _bleTransport;
            int profile = onBle ? IpProfileDatagram : BleProfileGatt;
            const uint migrationRef = 0x3F3Fu;
            uint endpointToken = 0xE9003F3Fu;

            Check(Mcl.mclx_contact_record_offer(_node, migrationRef, candidate, profile,
                                                endpointToken, 30) == 0, "offer recorded");
            int flags = Mcl.FlagSequence | Mcl.FlagFrameCheck;
            if (before.SessionValid != 0) { flags |= Mcl.FlagSession; }
            Check(Mcl.mclx_send_transport_offer(_node, flags, 1, migrationRef, candidate,
                                                profile, endpointToken, 30) == Mcl.SdkOk,
                  "TRANSPORT_OFFER sent");

            var accept = AwaitObject(Mcl.KindTransportAccept, 8000);
            Check(accept != null, "board accepted");
            if (accept != null)
            {
                Check(Mcl.mclx_contact_agree(_node, migrationRef, candidate, profile,
                                             accept.Rx.SessionRef) == 0, "acceptance applied");
            }

            // The candidate is never probed. Give up on it.
            Check(Mcl.mclx_contact_abandon_migration(_node) == 0, "migration abandoned");

            var after = Snapshot();
            Check(after.State == 1 /* ACTIVE */, "the contact is ACTIVE again");
            Check(after.ActiveTransport == oldTransport,
                  $"still carried on {Mcl.TransportName(oldTransport)}");
            Check(after.SessionRef == before.SessionRef,
                  "abandoning did not unbind the session reference");

            Step("the old path still carries traffic");
            Check(Mcl.mclx_send_presence(_node, Mcl.ClassData,
                                         Mcl.FlagSequence | Mcl.FlagFrameCheck | Mcl.FlagSession, 1)
                  == Mcl.SdkOk, "PRESENCE sent on the old path");
            var reply = Receive(6000);
            Check(reply != null && reply.Status == Mcl.SdkOk, "board answered on the old path");
            Check(reply == null || reply.Transport == oldTransport,
                  "the answer came back on the old path");

            /*
             * The board's counters are the run's second, independent record, and
             * RESET zeroes them. They are captured HERE, before the reset, or the
             * summary would report a clean board that had simply been wiped.
             */
            _finalBoardStatus = _board.Status(6000);

            /*
             * The board is still holding the transaction it accepted, because it
             * was never told the migration was over. There is no abort message,
             * by design: on a lossy medium an abort can itself be lost, and the
             * peers would then disagree about whether it happened. Returning both
             * ends to a fresh contact is a deployment action, not a protocol one.
             */
            _board.Send("RESET");
            Check(_board.Await("MCLDUAL RESET OK", 4000) != null,
                  "board returned to a fresh contact");
        }

        // ------------------------------------------------------------ helpers

        private static Received AwaitObject(int kind, int timeoutMs)
        {
            var deadline = DateTime.UtcNow.AddMilliseconds(timeoutMs);
            while (DateTime.UtcNow < deadline)
            {
                int remaining = (int)(deadline - DateTime.UtcNow).TotalMilliseconds;
                var r = Receive(remaining <= 0 ? 1 : remaining);
                if (r == null) { return null; }
                if (!r.IsHandoff && r.Status == Mcl.SdkOk && r.Rx.Kind == kind) { return r; }
                _board?.Note($"discarded while waiting for kind {kind}: " +
                             $"handoff={r.IsHandoff} status={Mcl.SdkStatusName(r.Status)} " +
                             $"kind={(r.IsHandoff ? -1 : r.Rx.Kind)}");
            }
            return null;
        }

        private static Received AwaitHandoff(int operation, int timeoutMs)
        {
            var deadline = DateTime.UtcNow.AddMilliseconds(timeoutMs);
            while (DateTime.UtcNow < deadline)
            {
                int remaining = (int)(deadline - DateTime.UtcNow).TotalMilliseconds;
                var r = Receive(remaining <= 0 ? 1 : remaining);
                if (r == null) { return null; }
                if (r.IsHandoff && r.Status == Mcl.SdkOk && r.Op == operation) { return r; }
                _board?.Note($"discarded while waiting for op {operation}: " +
                             $"handoff={r.IsHandoff} status={Mcl.SdkStatusName(r.Status)} " +
                             $"op={(r.IsHandoff ? r.Op : -1)}");
            }
            return null;
        }

        // ------------------------------------------------------------ summary

        private static void Summary()
        {
            Console.WriteLine();
            Console.WriteLine("Board's own record");
            Console.WriteLine("------------------");
            var status = _finalBoardStatus ?? _board?.Status(6000);
            if (status != null && status.Count > 0)
            {
                Console.WriteLine("  " + string.Join(" ", status.Select(kv => $"{kv.Key}={kv.Value}")));

                long logDropped = Board.Counter(status, "logdrop");
                Check(logDropped == 0,
                      $"the board's serial record is complete rather than merely quiet " +
                      $"(logdrop={logDropped})");
            }
            else
            {
                Check(false, "board answered a final STATUS");
            }

            Console.WriteLine();
            Console.WriteLine("Host's record");
            Console.WriteLine("-------------");
            Console.WriteLine($"  BLE frames sent={_ble?.FramesSent} fragments received={_ble?.Fragments} " +
                              $"reassembly refusals={_ble?.ReassemblyRefusals}");
            Console.WriteLine($"  UDP datagrams sent={_ip?.DatagramsSent} received={_ip?.DatagramsReceived} " +
                              $"refused by the binding={_ip?.DatagramsRefused}");
            Console.WriteLine($"  frames dropped on purpose by the host={_hostDroppedFrames}");
            Console.WriteLine($"  board serial bytes read={_board?.BytesRead}");

            Check(_ble == null || _ble.ReassemblyRefusals == 0,
                  "no BLE fragment was refused by reassembly");
            Check(_ip == null || _ip.DatagramsRefused == 0,
                  "no UDP datagram was refused by the IP binding");

            Console.WriteLine();
            Console.WriteLine($"{_checks} checks, {_failed} failed");
        }
    }
}
