import asyncio
import sys
from types import SimpleNamespace
sys.path.insert(0, 'mcl-sdk/hardware/windows-bench')
from bench import Bench
async def main():
    b = Bench(SimpleNamespace(dll='tmp/audit-20260909-android/windows-build/Release/mcl_windows_bench.dll', role=1))
    b.generation=1
    await b.activate(1, 0, 0xA5C30F17)
    await asyncio.sleep(8)
    await b.close_radio()
    b.dll.bench_destroy(b.handle)
asyncio.run(main())
