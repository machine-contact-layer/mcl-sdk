import argparse
from pathlib import Path
import struct
import zipfile
from datetime import datetime, timezone

p = argparse.ArgumentParser()
p.add_argument('archive')
p.add_argument('--extract', action='store_true')
a = p.parse_args()
archive = Path(a.archive)
with zipfile.ZipFile(archive) as z:
    names = [n for n in z.namelist() if 'snoop' in n.lower() or 'snooz' in n.lower()
             or 'bt_hci_20260909' in n.lower()]
    print('Trace files:', names)
    for name in names:
        data = z.read(name)
        if not data.startswith(b'btsnoop\0'):
            continue
        if a.extract:
            (archive.parent / Path(name).name).write_bytes(data)
        print('TRACE', name, 'bytes', len(data), 'version/link', struct.unpack('>II', data[8:16]))
        offset = 16
        connections = {}
        while offset + 24 <= len(data):
            original, included, flags, dropped, stamp = struct.unpack('>IIIIQ', data[offset:offset+24])
            packet = data[offset+24:offset+24+included]
            offset += 24 + included
            if len(packet) != included: raise ValueError('Truncated record')
            time = datetime.fromtimestamp((stamp - 0x00dcddb30f2f8000)/1e6, timezone.utc).isoformat()
            if len(packet) >= 15 and packet[:2] == b'\x04\x3e' and packet[3] in (1, 10):
                handle = int.from_bytes(packet[5:7], 'little')
                address = ':'.join(f'{v:02X}' for v in packet[9:15][::-1])
                connections[handle] = address
                print(time, 'CONNECT', hex(handle), 'status', packet[4], 'role', packet[7], 'peer', address)
            if len(packet) >= 9 and packet[0] == 2:
                handle = int.from_bytes(packet[1:3], 'little') & 0xfff
                pb = (int.from_bytes(packet[1:3], 'little') >> 12) & 3
                cid = int.from_bytes(packet[7:9], 'little')
                if pb != 1 and cid == 4:
                    print(time, 'ATT', 'RX' if flags & 1 else 'TX', hex(handle),
                          connections.get(handle, '?'), packet[9:].hex())
