import asyncio, ctypes as C, json, sys, wave
from pathlib import Path
from types import SimpleNamespace
sys.path.insert(0, str(Path.cwd() / 'mcl-sdk/hardware/windows-bench'))
from bench import Bench
async def main():
    bench = Bench(SimpleNamespace(dll=sys.argv[1], role=1))
    events=[]
    with wave.open(sys.argv[2],'rb') as wav:
        assert wav.getframerate()==48000 and wav.getsampwidth()==2 and wav.getnchannels()==1
        samples=0
        while True:
            raw=wav.readframes(2048)
            if not raw: break
            pcm=(C.c_int16*(len(raw)//2)).from_buffer_copy(raw)
            out=(C.c_uint8*17)(); info=(C.c_uint32*3)()
            rc=bench.dll.bench_audio(bench.handle,pcm,len(pcm),out,info)
            samples+=len(pcm)
            if rc in (1,2):
                events.append({'polled_at_sample':samples,'result':'CONTACT' if rc==1 else 'HEARD_UNRECOVERED','hex':bytes(out[:info[0]]).hex() if rc==1 else None})
    bench.dll.bench_destroy(bench.handle)
    print(json.dumps({'samples':samples,'events':events},indent=2))
asyncio.run(main())
