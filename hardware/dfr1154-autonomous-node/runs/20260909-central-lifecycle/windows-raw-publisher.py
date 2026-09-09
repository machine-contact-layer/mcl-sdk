import asyncio, sys, uuid
from types import SimpleNamespace
sys.path.insert(0, 'mcl-sdk/hardware/windows-bench')
from bench import Bench, SERVICE, winbuffer
from winrt.windows.devices.bluetooth.advertisement import BluetoothLEAdvertisementPublisher, BluetoothLEAdvertisementDataSection
from winrt.windows.devices.bluetooth.genericattributeprofile import GattServiceProviderAdvertisingParameters
async def main():
 b=Bench(SimpleNamespace(dll='tmp/audit-20260909-android/windows-build/Release/mcl_windows_bench.dll',role=1))
 b.generation=1
 await b.activate(1,0,0xA5C30F17)
 if b.provider:
  b.provider.stop_advertising()
  p=GattServiceProviderAdvertisingParameters();p.is_connectable=True;p.is_discoverable=True
  b.provider.start_advertising_with_parameters(p)
 publisher=BluetoothLEAdvertisementPublisher()
 section=BluetoothLEAdvertisementDataSection()
 section.data_type=0x21
 section.data=winbuffer(uuid.UUID(SERVICE).bytes[::-1]+(0xA5C30F17).to_bytes(8,'big'))
 publisher.advertisement.data_sections.append(section)
 publisher.add_status_changed(lambda _,e:print('PUBLISHER',e.status,e.error,flush=True))
 publisher.start()
 await asyncio.sleep(35)
 publisher.stop()
 await b.close_radio()
 b.dll.bench_destroy(b.handle)
asyncio.run(main())
