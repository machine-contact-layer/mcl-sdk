import argparse
from pathlib import Path
import subprocess
import time
import serial

p = argparse.ArgumentParser()
p.add_argument('name')
p.add_argument('--duration', type=int, default=120)
p.add_argument('--start-android', action='store_true')
p.add_argument('--windows', action='store_true')
p.add_argument('--windows-input', type=int, default=18)
p.add_argument('--windows-output', type=int, default=14)
a = p.parse_args()
directory = Path(__file__).resolve().parent
adb = ['C:/Users/marsm/rdb/adb.exe', '-s', '10.172.41.41:5555']
seen = set()
next_poll = 0
with (directory / (a.name + '-board.log')).open('w', encoding='utf-8', newline='\n') as board, \
     (directory / (a.name + '-android.log')).open('w', encoding='utf-8', newline='\n') as android, \
     serial.Serial('COM3', 921600, timeout=0.05) as port:
    time.sleep(0.5)
    port.reset_input_buffer()
    port.write(f'CONFIG 2 {a.duration * 1000} 0 0 100 3 1\n'.encode())
    time.sleep(0.3)
    port.write(b'ARM\n')
    windows = None
    if a.windows:
        windows_log = (directory / (a.name + '-windows.log')).open('w', encoding='utf-8')
        windows = subprocess.Popen(['python', '-u', 'mcl-sdk/hardware/windows-bench/bench.py', '--dll', str(directory / 'windows-build/Release/mcl_windows_bench.dll'), '--duration', str(a.duration), '--role', '0', '--input', str(a.windows_input), '--output', str(a.windows_output), '--admit'], stdout=windows_log, stderr=subprocess.STDOUT)
    if a.start_android:
        subprocess.run(adb + ['shell', 'am', 'broadcast', '-a', 'org.mcl.bench.CMD', '-p', 'org.mcl.bench', '--es', 'cmd', "'machine start responder'"], check=True, timeout=5)
    end = time.monotonic() + a.duration + 8
    while time.monotonic() < end:
        line = port.readline().decode('utf-8', errors='replace').strip()
        if line:
            board.write(line + '\n'); board.flush()
            if line.startswith(('MCLAUTO', 'MCL_ALLOC_FAIL')):
                print('BOARD ' + line, flush=True)
        if time.monotonic() >= next_poll:
            next_poll = time.monotonic() + 1
            result = subprocess.run(adb + ['logcat', '-d', '-s', 'MCLBENCH'], capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=5)
            for line in result.stdout.splitlines():
                if line in seen: continue
                seen.add(line)
                android.write(line + '\n'); android.flush()
                if any(word in line for word in ['MACHINE', 'ATT write', 'BLE frame', 'BLE central', 'BLE advertising']):
                    print('ANDROID ' + line, flush=True)
                if 'MACHINE event=2 ' in line:
                    reply = subprocess.run(adb + ['shell', 'am', 'broadcast', '-a', 'org.mcl.bench.CMD', '-p', 'org.mcl.bench', '--es', 'cmd', "'machine admit'"], capture_output=True, text=True, timeout=5)
                    android.write('HOST policy admit only after POLICY_REQUIRED; adb_rc=' + str(reply.returncode) + '\n')
    port.write(b'RESULT\n')
    time.sleep(1)
    tail = port.read_all().decode('utf-8', errors='replace')
    board.write(tail)
    print(tail, flush=True)
    if windows:
        windows.wait(timeout=20)
        windows_log.close()
