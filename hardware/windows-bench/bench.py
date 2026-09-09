"""Windows physical MCL facade port. No peer locator is accepted by this CLI."""
import argparse
import asyncio
import ctypes as C
import hashlib
import os
from pathlib import Path
import queue
import time
import uuid
import wave

import numpy as np
import sounddevice as sd
from bleak import BleakClient, BleakScanner
from winrt.windows.devices.bluetooth.genericattributeprofile import (
    GattServiceProvider, GattServiceProviderAdvertisingParameters,
    GattLocalCharacteristicParameters, GattCharacteristicProperties,
    GattProtectionLevel, GattWriteOption,
)
from winrt.windows.storage.streams import DataWriter

SERVICE = '6d636c00-0001-4d43-4c00-6d636c626c65'
RX = '6d636c00-0002-4d43-4c00-6d636c626c65'
TX = '6d636c00-0003-4d43-4c00-6d636c626c65'
U8 = C.POINTER(C.c_uint8)
VOID = C.c_void_p
CLOCK = C.CFUNCTYPE(C.c_uint32, VOID)
RANDOM = C.CFUNCTYPE(C.c_int, VOID, U8, C.c_size_t)
SEND = C.CFUNCTYPE(C.c_int32, VOID, C.c_uint8, U8, C.c_size_t)
FLAG = C.CFUNCTYPE(C.c_int, VOID)
OPEN = C.CFUNCTYPE(C.c_int, VOID, C.c_uint8, C.c_uint8, C.c_uint32, C.c_uint32)
CLOSE = C.CFUNCTYPE(None, VOID, C.c_uint8)


class Platform(C.Structure):
    _fields_ = [('clock', CLOCK), ('random', RANDOM), ('send', SEND),
                ('busy', FLAG), ('self_tx', FLAG), ('open', OPEN),
                ('close', CLOSE), ('policy', VOID), ('user', VOID)]


def buffer(data):
    return (C.c_uint8 * len(data)).from_buffer_copy(data)


def winbuffer(data):
    writer = DataWriter()
    writer.write_bytes(data)
    return writer.detach_buffer()


def log(message):
    print(f'{time.monotonic():.3f} WINDOWS {message}', flush=True)


class Bench:
    def __init__(self, args):
        self.args = args
        self.loop = asyncio.get_running_loop()
        self.dll = C.CDLL(str(Path(args.dll).resolve()))
        signatures = {
            'create': ([C.POINTER(Platform), C.c_uint32, C.c_int], VOID),
            'destroy': ([VOID], None), 'start': ([VOID], C.c_int),
            'poll': ([VOID, C.POINTER(C.c_uint32)], C.c_int),
            'ready': ([VOID], C.c_int), 'refused': ([VOID], C.c_int),
            'admit': ([VOID], C.c_int), 'state': ([VOID], C.c_char_p),
            'reset_audio': ([VOID], C.c_int),
            'receive': ([VOID, C.c_int, U8, C.c_size_t], C.c_int),
            'audio': ([VOID, C.POINTER(C.c_int16), C.c_size_t, U8, C.POINTER(C.c_uint32)], C.c_int),
            'modulate': ([U8, C.c_size_t, C.POINTER(C.c_int16), C.c_size_t], C.c_int),
            'fragment': ([U8, C.c_size_t, C.c_size_t, U8], C.c_int),
            'reassemble': ([VOID, U8, C.c_size_t, U8], C.c_int),
            'reset_fragments': ([VOID], None),
        }
        for name, (argtypes, result) in signatures.items():
            fn = getattr(self.dll, 'bench_' + name)
            fn.argtypes, fn.restype = argtypes, result
        self.audio_queue = queue.Queue(maxsize=128)
        self.audio_drops = 0
        self.capture_blocks = []
        self.capture_samples = 0
        self.capture_limit = min(getattr(args, 'duration', 120) + 5, 300) * 48000
        self.max_audio_queue_ms = 0.0
        self.self_tx = False
        self.busy = False
        self.last_busy_report = False
        self.ready = False
        self.closed = False
        self.generation = 0
        self.client = self.provider = self.rx = self.tx = None
        self.tasks = set()
        self.tx_queue = asyncio.Queue(maxsize=16)
        self.established = []
        self.rejections = 0
        self.first_slot = getattr(args, 'first_slot', None)
        self.source = int.from_bytes(os.urandom(4), 'little') or 1
        self.callbacks = (CLOCK(lambda _: int(time.monotonic() * 1000) & 0xffffffff),
                          RANDOM(self.random), SEND(self.send),
                          FLAG(self.medium_busy), FLAG(lambda _: int(self.self_tx)),
                          OPEN(self.open_candidate), CLOSE(self.close_candidate))
        self.platform = Platform(*self.callbacks, None, None)
        self.handle = self.dll.bench_create(C.byref(self.platform), self.source, args.role)
        if not self.handle:
            raise RuntimeError('canonical machine/listener initialization refused')
        log(f'source_ref={self.source:08X} provenance=LOCAL deployment=MCL-REFERENCE-DEPLOYMENT-1')
        log('dll_sha256=' + hashlib.sha256(Path(args.dll).read_bytes()).hexdigest())

    def medium_busy(self, _):
        if self.busy != self.last_busy_report:
            log(f'AP carrier_busy={int(self.busy)}')
            self.last_busy_report = self.busy
        return int(self.busy)

    def random(self, _, out, size):
        if size == 1 and self.first_slot is not None:
            log(f'LAB first contention draw={self.first_slot}; subsequent RNG=OS')
            out[0] = self.first_slot
            self.first_slot = None
        else:
            C.memmove(out, os.urandom(size), size)
        return 0

    def task(self, coroutine):
        task = self.loop.create_task(coroutine)
        self.tasks.add(task)
        task.add_done_callback(self.tasks.discard)
        return task

    def send(self, _, transport, pointer, size):
        data = C.string_at(pointer, size)
        if transport == 1:
            pcm = (C.c_int16 * 100000)()
            count = self.dll.bench_modulate(buffer(data), size, pcm, len(pcm))
            if count <= 0:
                return -1
            self.self_tx = True
            try:
                log(f'AP TX bytes={size} hex={data.hex()} samples={count}')
                sd.play(np.ctypeslib.as_array(pcm)[:count], 48000,
                        device=self.args.output, blocking=True)
                return 0
            except Exception as error:
                log(f'AP TX uncertain error={error}')
                return 1
            finally:
                self.self_tx = False
        if transport == 3 and self.ready:
            try:
                self.tx_queue.put_nowait((self.generation, data))
                return 1  # Queue admission, not delivery.
            except asyncio.QueueFull:
                return -1
        return -1

    def open_candidate(self, _, transport, profile, peer, local):
        log(f'CANDIDATE open transport={transport} profile={profile} peer_token={peer:08X} local_token={local:08X}')
        if transport != 3 or profile != 1 or not (peer or local):
            return 2
        self.generation += 1
        self.task(self.activate(self.generation, peer, local))
        return 1

    def close_candidate(self, _, transport):
        log(f'CANDIDATE close transport={transport}')
        self.ready = False
        self.generation += 1
        self.task(self.close_radio())

    async def close_radio(self):
        client, provider = self.client, self.provider
        self.client = self.provider = self.rx = self.tx = None
        if provider is not None:
            try:
                provider.stop_advertising()
            except OSError:
                pass
        if client is not None and client.is_connected:
            await client.disconnect()
        self.dll.bench_reset_fragments(self.handle)

    async def activate(self, generation, peer, local):
        try:
            if peer:
                expected = peer.to_bytes(8, 'big')
                device = await BleakScanner.find_device_by_filter(
                    lambda device, ad: ad.service_data.get(SERVICE) == expected, timeout=25)
                if device is None or generation != self.generation:
                    raise RuntimeError('exact UUID/token scan deadline')
                log(f'BLE exact match address={device.address} provenance=FROM_BEARER')
                self.client = BleakClient(device)
                await self.client.connect()
                rx = self.client.services.get_characteristic(RX)
                tx = self.client.services.get_characteristic(TX)
                if not rx or not tx or 'write-without-response' not in rx.properties or 'notify' not in tx.properties:
                    raise RuntimeError('required GATT properties missing')
                await self.client.start_notify(tx, lambda _, value: self.fragment_received(bytes(value)))
                self.rx = rx
                self.mark_ready(generation)
            else:
                result = await GattServiceProvider.create_async(uuid.UUID(SERVICE))
                if int(result.error) != 0:
                    raise RuntimeError(f'GATT provider error={result.error}')
                self.provider = result.service_provider
                params = GattLocalCharacteristicParameters()
                params.characteristic_properties = GattCharacteristicProperties.WRITE_WITHOUT_RESPONSE | GattCharacteristicProperties.WRITE
                params.write_protection_level = GattProtectionLevel.PLAIN
                result = await self.provider.service.create_characteristic_async(uuid.UUID(RX), params)
                if int(result.error) != 0:
                    raise RuntimeError(f'RX creation error={result.error}')
                self.rx = result.characteristic
                def written(_, args):
                    deferral = args.get_deferral()
                    self.loop.call_soon_threadsafe(self.task, self.handle_write(args, deferral, generation))
                self.rx.add_write_requested(written)
                params = GattLocalCharacteristicParameters()
                params.characteristic_properties = GattCharacteristicProperties.NOTIFY
                params.read_protection_level = GattProtectionLevel.PLAIN
                result = await self.provider.service.create_characteristic_async(uuid.UUID(TX), params)
                if int(result.error) != 0:
                    raise RuntimeError(f'TX creation error={result.error}')
                self.tx = result.characteristic
                self.tx.add_subscribed_clients_changed(lambda *_: self.loop.call_soon_threadsafe(self.subscribed, generation))
                self.provider.add_advertisement_status_changed(lambda _, event: log(f'BLE advertising status={event.status} error={event.error}'))
                advertising = GattServiceProviderAdvertisingParameters()
                advertising.is_connectable = True
                advertising.is_discoverable = True
                advertising.service_data = winbuffer(local.to_bytes(8, 'big'))
                self.provider.start_advertising_with_parameters(advertising)
                log(f'BLE advertising token={local:08X} provenance=LOCAL')
                await asyncio.sleep(1)
                if generation == self.generation and int(self.provider.advertisement_status) != 2:
                    raise RuntimeError('Windows cannot advertise the complete required service-data beacon')
        except Exception as error:
            log(f'BLE activation failed {type(error).__name__}: {error}')
            if generation == self.generation:
                log(f'CANDIDATE refused status={self.dll.bench_refused(self.handle)}')

    def subscribed(self, generation):
        if not self.closed and generation == self.generation and self.tx is not None and len(self.tx.subscribed_clients):
            self.mark_ready(generation)

    def mark_ready(self, generation):
        if not self.closed and generation == self.generation and not self.ready:
            self.ready = True
            log(f'CANDIDATE ready status={self.dll.bench_ready(self.handle)}')

    async def handle_write(self, args, deferral, generation):
        try:
            request = await args.get_request_async()
            if request is not None:
                if request.offset != 0 or generation != self.generation:
                    request.respond_with_protocol_error(7)
                else:
                    self.fragment_received(bytes(request.value))
                    if request.option == GattWriteOption.WRITE_WITH_RESPONSE:
                        request.respond()
        finally:
            deferral.complete()

    def fragment_received(self, data):
        if self.closed:
            return
        out = (C.c_uint8 * 2048)()
        used = self.dll.bench_reassemble(self.handle, buffer(data), len(data), out)
        if used < 0:
            self.rejections += 1
            log('BLE fragment rejected')
        elif used:
            log(f'BLE RX frame={bytes(out[:used]).hex()}')
            self.dll.bench_receive(self.handle, 3, out, used)

    async def transmit_worker(self):
        while True:
            generation, data = await self.tx_queue.get()
            try:
                for index in range((len(data) + 18) // 19):
                    if generation != self.generation or not self.ready:
                        raise RuntimeError('candidate closed during queued transmission')
                    out = (C.c_uint8 * 20)()
                    used = self.dll.bench_fragment(buffer(data), len(data), index, out)
                    if used <= 0:
                        raise RuntimeError('canonical fragment refused')
                    pdu = bytes(out[:used])
                    if self.client is not None:
                        await self.client.write_gatt_char(self.rx, pdu, response=False)
                    elif self.tx is not None:
                        results = await self.tx.notify_value_async(winbuffer(pdu))
                        if not len(results) or any(int(result.status) != 0 for result in results):
                            raise RuntimeError('notification delivery status refused')
                    else:
                        raise RuntimeError('no BLE endpoint')
                    await asyncio.sleep(0.02)
                log(f'BLE TX queued frame={data.hex()}')
            except Exception as error:
                log(f'BLE queued TX failed error={error}')
            finally:
                self.tx_queue.task_done()

    def captured(self, pcm, frames, timing, status):
        if getattr(self.args, 'capture', None) and self.capture_samples + frames <= self.capture_limit:
            self.capture_blocks.append(pcm.copy())
            self.capture_samples += frames
        if status:
            self.audio_drops += 1
        try:
            self.audio_queue.put_nowait((pcm.copy(), self.self_tx, time.monotonic()))
        except queue.Full:
            self.audio_drops += 1

    async def run(self):
        worker = self.task(self.transmit_worker())
        try:
            with sd.InputStream(samplerate=48000, channels=1, dtype='int16', blocksize=2048,
                                device=self.args.input, latency='low', callback=self.captured) as stream:
                log(f'AUDIO input_latency={stream.latency} device={sd.query_devices(self.args.input, "input")["name"]}')
                log(f'MACHINE start status={self.dll.bench_start(self.handle)} input={self.args.input} output={self.args.output}')
                end = time.monotonic() + self.args.duration
                while time.monotonic() < end:
                    for _ in range(16):
                        try:
                            pcm, self_echo, captured_at = self.audio_queue.get_nowait()
                        except queue.Empty:
                            break
                        self.max_audio_queue_ms = max(self.max_audio_queue_ms, (time.monotonic() - captured_at) * 1000)
                        if self_echo:
                            self.dll.bench_reset_audio(self.handle)
                            self.busy = False
                            continue
                        out, info = (C.c_uint8 * 17)(), (C.c_uint32 * 3)()
                        result = self.dll.bench_audio(self.handle, pcm.ctypes.data_as(C.POINTER(C.c_int16)), len(pcm), out, info)
                        self.busy = result == 3 and not self_echo
                        if result == 2 and not self_echo:
                            log(f'AP HEARD_UNRECOVERED captured_at={captured_at:.3f}')
                        if result == 1 and not self_echo:
                            log(f'AP RX bytes={info[0]} hex={bytes(out[:info[0]]).hex()} contacts={info[2]}')
                            self.dll.bench_receive(self.handle, 1, out, info[0])
                    event = (C.c_uint32 * 6)()
                    status = self.dll.bench_poll(self.handle, event)
                    if status or event[0]:
                        log(f'MACHINE event={event[0]} transport={event[1]} profile={event[2]} peer_ref={event[3]:08X} session_ref={event[4]:08X} status={event[5]} poll={status}')
                    if event[0] == 2 and self.args.admit:
                        log(f'POLICY explicit laboratory admission status={self.dll.bench_admit(self.handle)}')
                    if event[0] == 3:
                        self.established.append(list(event))
                    await asyncio.sleep(0.01)
        finally:
            if getattr(self.args, 'capture', None):
                with wave.open(self.args.capture, 'wb') as capture:
                    capture.setnchannels(1)
                    capture.setsampwidth(2)
                    capture.setframerate(48000)
                    for block in self.capture_blocks:
                        capture.writeframesraw(block.tobytes())
                log(f'AUDIO capture_samples={self.capture_samples} path={self.args.capture}')
            log(f'RESULT state={self.dll.bench_state(self.handle).decode()} established={len(self.established)} audio_drops={self.audio_drops} fragment_rejections={self.rejections} max_audio_queue_ms={self.max_audio_queue_ms:.1f}')
            self.ready = False
            self.closed = True
            self.generation += 1
            worker.cancel()
            for task in list(self.tasks):
                task.cancel()
            await asyncio.gather(*list(self.tasks), return_exceptions=True)
            await self.close_radio()
            self.dll.bench_destroy(self.handle)


async def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dll', required=True)
    parser.add_argument('--duration', type=int, default=120)
    parser.add_argument('--role', type=int, choices=[0, 1], default=1)
    parser.add_argument('--input', type=int, default=None)
    parser.add_argument('--output', type=int, default=None)
    parser.add_argument('--first-slot', type=int, choices=range(8), help='disclosed laboratory first contention draw; later draws remain OS random')
    parser.add_argument('--capture', help='retain raw microphone PCM, including local TX; bounded to 300 seconds')
    parser.add_argument('--admit', action='store_true', help='explicit laboratory policy for reachable strangers')
    args = parser.parse_args()
    await Bench(args).run()


if __name__ == '__main__':
    asyncio.run(main())
