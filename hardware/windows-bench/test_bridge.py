"""Exercise the host ABI before allowing it to supply physical evidence."""
import asyncio
import ctypes as C
from pathlib import Path
import sys
from types import SimpleNamespace
from bench import Bench, buffer


async def main():
    bench = Bench(SimpleNamespace(dll=sys.argv[1], role=1))
    lib, handle = bench.dll, bench.handle
    try:
        payload = bytes(range(17))
        pcm = (C.c_int16 * 100000)()
        count = lib.bench_modulate(buffer(payload), len(payload), pcm, len(pcm))
        assert count > 0
        samples = [0] * 4096 + list(pcm[:count]) + [0] * 32000
        recovered = []
        for offset in range(0, len(samples), 2048):
            block = (C.c_int16 * 2048)(*(samples[offset:offset + 2048]))
            out, info = (C.c_uint8 * 17)(), (C.c_uint32 * 3)()
            result = lib.bench_audio(handle, block, len(block), out, info)
            if result == 1:
                recovered.append(bytes(out[:info[0]]))
        assert recovered == [payload], recovered
        payload = bytes(range(1, 41))
        pdus = []
        for index in range(3):
            out = (C.c_uint8 * 20)()
            used = lib.bench_fragment(buffer(payload), len(payload), index, out)
            assert used > 0
            pdus.append(bytes(out[:used]))
        frame = (C.c_uint8 * 2048)()
        recovered = []
        for pdu in pdus:
            used = lib.bench_reassemble(handle, buffer(pdu), len(pdu), frame)
            assert used >= 0
            if used:
                recovered.append(bytes(frame[:used]))
        assert recovered == [payload]
        lib.bench_reset_fragments(handle)
        assert lib.bench_reassemble(handle, buffer(pdus[0]), len(pdus[0]), frame) == 0
        assert lib.bench_reassemble(handle, buffer(pdus[2]), len(pdus[2]), frame) == -1
        print('PASS: canonical PCM exact recovery; 40-byte three-fragment exact recovery; missing middle refused')
    finally:
        lib.bench_destroy(handle)


asyncio.run(main())
