// The two radios, and nothing else.
//
// Each class here owns a medium: a GATT connection or a UDP socket. Neither
// knows what a Link frame is. They move opaque byte strings and report, in the
// three-way vocabulary the SDK transmit callback requires, whether the bytes
// left this machine.
//
// THE THREE-WAY RETURN IS NOT COSMETIC
//
// 0 means accepted, negative means definitely nothing left, positive means the
// radio cannot say. The SDK uses the difference to decide whether sending a
// COMMIT became irrevocable, so a radio that reports certainty it does not have
// would either strand a contact in COMMITTING over a frame nobody saw, or roll
// back a commit the peer may have acted on. Each class below returns positive
// for exactly the cases it genuinely cannot resolve.

using System;
using System.Collections.Concurrent;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using Windows.Devices.Bluetooth;
using Windows.Devices.Bluetooth.Advertisement;
using Windows.Devices.Bluetooth.GenericAttributeProfile;
using Windows.Devices.Enumeration;
using Windows.Storage.Streams;

namespace Mcl.Sdk.DualTransport
{
    internal sealed class Inbound
    {
        public int Transport;
        public byte[] Frame;
    }

    /*
     * One queue for both radios.
     *
     * A frame's arrival transport travels with it, because the SDK checks it and
     * that check is the entire reason this experiment exists. Losing track of
     * which medium a frame came in on would silently turn the strictest test
     * here into no test at all.
     */
    internal sealed class InboundQueue
    {
        private readonly BlockingCollection<Inbound> _q = new();

        public void Add(int transport, byte[] frame) =>
            _q.Add(new Inbound { Transport = transport, Frame = frame });

        public Inbound Take(int timeoutMs) =>
            _q.TryTake(out var item, timeoutMs) ? item : null;

        public void Drain()
        {
            while (_q.TryTake(out _)) { }
        }

        public int Count => _q.Count;
    }

    internal sealed class BleRadio : IDisposable
    {
        private readonly InboundQueue _queue;
        private readonly string _serviceUuid;
        private readonly string _rxUuid;
        private readonly string _txUuid;
        private readonly int _transportId;

        private BluetoothLEDevice _device;
        private GattDeviceService _service;
        private GattCharacteristic _rx;
        private GattCharacteristic _tx;
        private IntPtr _reassembler;

        public int Fragments { get; private set; }
        public int ReassemblyRefusals { get; private set; }
        public int FramesSent { get; private set; }
        public bool Connected => _rx != null && _tx != null;

        public BleRadio(InboundQueue queue, string serviceUuid, string rxUuid, string txUuid)
        {
            _queue = queue;
            _serviceUuid = serviceUuid;
            _rxUuid = rxUuid;
            _txUuid = txUuid;
            _transportId = Mcl.mclx_transport_ble();
            _reassembler = Marshal.AllocHGlobal(Mcl.mclx_reassembler_size());
            Mcl.mclx_ble_reassembler_reset(_reassembler);
        }

        public async Task<ulong> ScanAsync(int timeoutSeconds)
        {
            var watcher = new BluetoothLEAdvertisementWatcher
            {
                ScanningMode = BluetoothLEScanningMode.Active
            };
            watcher.AdvertisementFilter.Advertisement.ServiceUuids.Add(new Guid(_serviceUuid));

            var found = new TaskCompletionSource<ulong>();
            watcher.Received += (_, e) => found.TrySetResult(e.BluetoothAddress);
            watcher.Start();
            var winner = await Task.WhenAny(found.Task, Task.Delay(TimeSpan.FromSeconds(timeoutSeconds)));
            watcher.Stop();

            return winner == found.Task ? found.Task.Result : 0UL;
        }

        public async Task<bool> ConnectAsync(ulong address)
        {
            _device = await BluetoothLEDevice.FromBluetoothAddressAsync(address);
            if (_device == null) { return false; }

            // Windows will not open a GATT session to an unpaired peripheral,
            // and a device object obtained before pairing keeps failing after
            // the bond exists, so it is re-acquired.
            if (!_device.DeviceInformation.Pairing.IsPaired &&
                _device.DeviceInformation.Pairing.CanPair)
            {
                var custom = _device.DeviceInformation.Pairing.Custom;
                custom.PairingRequested += (_, request) => request.Accept();
                var result = await custom.PairAsync(DevicePairingKinds.ConfirmOnly,
                                                    DevicePairingProtectionLevel.None);
                Console.WriteLine($"      BLE pairing: {result.Status}");

                _device.Dispose();
                await Task.Delay(1500);
                _device = await BluetoothLEDevice.FromBluetoothAddressAsync(address);
                if (_device == null) { return false; }
            }

            GattDeviceServicesResult services = null;
            for (int attempt = 0; attempt < 6; attempt++)
            {
                try
                {
                    // Enumerating every service, rather than filtering by UUID,
                    // is what reliably drives Windows to establish the link.
                    services = await _device.GetGattServicesAsync(BluetoothCacheMode.Uncached);
                    if (services.Status == GattCommunicationStatus.Success &&
                        services.Services.Any(s => s.Uuid == new Guid(_serviceUuid)))
                    {
                        break;
                    }
                }
                catch (Exception ex)
                {
                    // A peripheral that has just started advertising is often
                    // not connectable for an attempt or two, which looks exactly
                    // like a real failure unless the HRESULT is printed.
                    Console.WriteLine($"      BLE discovery attempt {attempt + 1}: " +
                                      $"0x{ex.HResult:X8} {ex.Message.Trim()}");
                }
                await Task.Delay(2000);
            }

            _service = services?.Services.FirstOrDefault(s => s.Uuid == new Guid(_serviceUuid));
            if (_service == null) { return false; }

            _rx = (await _service.GetCharacteristicsForUuidAsync(new Guid(_rxUuid)))
                  .Characteristics.FirstOrDefault();
            _tx = (await _service.GetCharacteristicsForUuidAsync(new Guid(_txUuid)))
                  .Characteristics.FirstOrDefault();
            if (_rx == null || _tx == null) { return false; }

            int frameMax = Mcl.mclx_frame_max_size();
            var assembled = new byte[frameMax];

            _tx.ValueChanged += (_, e) =>
            {
                var fragment = new byte[e.CharacteristicValue.Length];
                DataReader.FromBuffer(e.CharacteristicValue).ReadBytes(fragment);
                Fragments++;

                // Reassembly is the binding's, never this file's.
                int n = Mcl.mclx_ble_reassemble(_reassembler, fragment, fragment.Length,
                                                assembled, assembled.Length);
                if (n > 0) { _queue.Add(_transportId, assembled.Take(n).ToArray()); }
                else if (n < 0) { ReassemblyRefusals++; }
                // n == 0 means more fragments are expected, which is normal.
            };

            var subscribed = await _tx.WriteClientCharacteristicConfigurationDescriptorAsync(
                GattClientCharacteristicConfigurationDescriptorValue.Notify);
            return subscribed == GattCommunicationStatus.Success;
        }

        /*
         * Fragment at the SMALLEST MTU Bluetooth LE permits, not at whatever
         * this connection negotiated. Windows negotiates a large one, which
         * would carry every frame in a single PDU and leave the fragmentation
         * path untested. The minimum is the case the scheme has to survive.
         */
        public int Send(byte[] frame)
        {
            if (_rx == null) { return -1; }   // no link: definitely nothing left

            int mtu = Mcl.mclx_ble_default_mtu();
            int count = Mcl.mclx_ble_fragment_count(frame.Length, mtu);
            if (count <= 0) { return -1; }

            for (int i = 0; i < count; i++)
            {
                var pdu = new byte[mtu];
                int n = Mcl.mclx_ble_fragment(frame, frame.Length, mtu, i, pdu, pdu.Length);
                if (n <= 0) { return i == 0 ? -1 : 1; }

                var writer = new DataWriter();
                writer.WriteBytes(pdu.Take(n).ToArray());
                GattCommunicationStatus status;
                try
                {
                    status = _rx.WriteValueAsync(writer.DetachBuffer(),
                                                 GattWriteOption.WriteWithoutResponse)
                                .AsTask().GetAwaiter().GetResult();
                }
                catch
                {
                    /*
                     * The write threw partway through a fragment burst. Earlier
                     * fragments are already out, so the peer may be holding part
                     * of a frame it can never complete. Bytes left this machine,
                     * so this is UNCERTAIN rather than "not sent".
                     */
                    return i == 0 ? -1 : 1;
                }
                if (status != GattCommunicationStatus.Success) { return i == 0 ? -1 : 1; }
                Thread.Sleep(12);   // let the stack drain; a burst is dropped, not queued
            }

            FramesSent++;
            return 0;
        }

        public void Dispose()
        {
            _service?.Dispose();
            _device?.Dispose();
            if (_reassembler != IntPtr.Zero)
            {
                Marshal.FreeHGlobal(_reassembler);
                _reassembler = IntPtr.Zero;
            }
        }
    }

    internal sealed class IpRadio : IDisposable
    {
        private readonly InboundQueue _queue;
        private readonly UdpClient _socket;
        private readonly IPEndPoint _peer;
        private readonly int _transportId;
        private readonly CancellationTokenSource _cancel = new();

        public int DatagramsSent { get; private set; }
        public int DatagramsReceived { get; private set; }
        public int DatagramsRefused { get; private set; }
        public IPEndPoint LocalEndPoint => (IPEndPoint)_socket.Client.LocalEndPoint;

        /*
         * The socket is bound to a specific local address, not to Any.
         *
         * On a machine with two wireless interfaces the routing table decides
         * which one unbound traffic leaves by, and the result then describes a
         * different link from the one being measured. This binding is also what
         * keeps the experiment off the laptop's ordinary network.
         */
        public IpRadio(InboundQueue queue, IPAddress bindAddress, IPEndPoint peer)
        {
            _queue = queue;
            _peer = peer;
            _transportId = Mcl.mclx_transport_ip();
            _socket = new UdpClient(new IPEndPoint(bindAddress, 0));
            Task.Run(ReceiveLoop);
        }

        private async Task ReceiveLoop()
        {
            while (!_cancel.IsCancellationRequested)
            {
                UdpReceiveResult result;
                try { result = await _socket.ReceiveAsync(_cancel.Token); }
                catch (OperationCanceledException) { return; }
                catch (ObjectDisposedException) { return; }
                catch (SocketException) { continue; }

                DatagramsReceived++;

                /*
                 * A datagram carries exactly one Link frame and nothing else.
                 * The check is the binding's; assuming whatever arrived is a
                 * frame is how framing confusion becomes semantic confusion.
                 */
                if (Mcl.mclx_ip_datagram_validate(result.Buffer, result.Buffer.Length) != 0)
                {
                    DatagramsRefused++;
                    continue;
                }
                _queue.Add(_transportId, result.Buffer);
            }
        }

        public int Send(byte[] frame)
        {
            try
            {
                int n = _socket.Send(frame, frame.Length, _peer);
                if (n != frame.Length) { return 1; }   // partial write: cannot say
                DatagramsSent++;
                return 0;
            }
            catch (SocketException)
            {
                // The stack refused the datagram, so it was never handed to the
                // driver. Nothing was transmitted.
                return -1;
            }
        }

        public void Dispose()
        {
            _cancel.Cancel();
            _socket.Dispose();
            _cancel.Dispose();
        }
    }
}
