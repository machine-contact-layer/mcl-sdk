import subprocess,time,serial,json
from pathlib import Path
root=Path(__file__).parent
adb=['C:/Users/marsm/rdb/adb.exe','-s','10.172.41.41:5555']
def cmd(text):
    return subprocess.run(adb+['shell','am','broadcast','-a','org.mcl.bench.CMD','-p','org.mcl.bench','--es','cmd',"'"+text+"'"],capture_output=True,timeout=5)
cmd('machine stop')
for mode in ('retry','stop'):
    with (root/f'closure-final-{mode}.log').open('w',encoding='utf-8') as log, serial.Serial('COM3',921600,timeout=.05) as port:
        port.write(b'CONFIG 6 20000 0 0 100 3 1\n'); time.sleep(.3); port.write(b'ARM\n')
        end=time.monotonic()+28; action_at=None; seen=0
        while time.monotonic()<end:
            line=port.readline().decode(errors='replace').strip()
            if line:
                log.write(line+'\n'); log.flush()
                if 'BLE found c0:00:00:00:00:01' in line:
                    action_at=time.monotonic()+1
            if action_at and time.monotonic()>=action_at:
                if mode=='stop':
                    port.write(b'STOP\n'); log.write('HOST STOP issued during worker attempt\n'); end=min(end,time.monotonic()+8)
                else:
                    r=cmd('emit PRESENCE 100'); log.write('HOST canonical PRESENCE playback requested during worker attempt rc='+str(r.returncode)+'\n')
                action_at=None; seen+=1
        port.write(b'RESULT\n'); time.sleep(.3); log.write(port.read_all().decode(errors='replace'))
    print(mode,'completed',flush=True)
t0=time.time(); phone=subprocess.check_output(adb+['shell','date','+%s.%N'],text=True).strip(); t1=time.time()
(root/'closure-clock.json').write_text(json.dumps({'host_epoch_before':t0,'android_epoch':phone,'host_epoch_after':t1},indent=2))
