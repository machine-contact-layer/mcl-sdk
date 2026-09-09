import json,re,sys
from pathlib import Path
root=Path(sys.argv[1]) if len(sys.argv)>1 else Path(__file__).parent
results={}
for cell in (1,2,3):
    name=f'closure-trace-cell{cell}'
    logs={k:(root/f'{name}-{k}.log').read_text(encoding='utf-8') for k in ('board','android','windows')}
    b,a,w=(logs[k] for k in ('board','android','windows'))
    own_b=f"{int(re.search(r'READY .*source_ref=(\d+)',b)[1]):08X}"
    own_a=re.search(r'AP TX_ATTEMPT hex=....([0-9A-F]{8})',a)[1]
    own_w=re.search(r'source_ref=([0-9A-F]{8})',w)[1]
    assert len({own_b,own_a,own_w})==3
    tx={};rx={}
    for k,log in logs.items():
        tx[k]=[bytes.fromhex(x) for x in re.findall(r'AP TX(?:_ATTEMPT)? (?:bytes=\d+ )?hex=([0-9a-fA-F]+)',log)]
        rx[k]=[bytes.fromhex(x) for x in re.findall(r'AP RX (?:bytes=\d+ )?hex=([0-9a-fA-F]+)',log)]
        assert tx[k], (name,k,'no transmission')
    offers={frame[6:12]:frame[2:6] for frames in tx.values() for frame in frames if frame[:2]==bytes.fromhex('1802')}
    accepts=[frame for frames in tx.values() for frame in frames if frame[:2]==bytes.fromhex('180a')]
    edges=set()
    for frame in accepts:
        assert frame[6:12] in offers, (name,'accept without recorded matching offer')
        edges.add((frame[2:6],offers[frame[6:12]]))
    assert not any((y,z) in edges and (z,x) in edges for x,y in edges for z in {v for edge in edges for v in edge}), (name,'three-edge cycle')
    ses=re.search(r'CONTACT_ESTABLISHED transport=3 profile=1 ses=([0-9A-F]{8})',b)[1]
    assert f'event=3 transport=3 profile=1 peer_ref={own_b} session_ref={ses} status=0' in a
    assert f'policy: peer_ref={own_a} transport=3 -> admit' in b and 'MACHINE policy admit status=0' in a
    assert any(f[12:16].hex().upper()==ses and f[2:6].hex().upper() in (own_a,own_b) for f in accepts)
    assert 'machine=MIGRATED' in b and 'log_dropped=0 ble_lost=0' in b
    assert 'established=0 audio_drops=0' in w
    results[name]={'source_refs':{'board':own_b,'android':own_a,'windows':own_w},'session':ses,'tx_frames':{k:len(v) for k,v in tx.items()},'rx_frames':{k:len(v) for k,v in rx.items()},'accept_proposals':len(accepts),'matching_pair_migrated':True,'no_three_edge_accept_cycle':True,'windows_heard_by': [k for k in ('board','android') if any(f[2:6].hex().upper()==own_w for f in rx[k])]}
# Competing proposals are permitted by AP section 8.1; later adoption is not.
b1=(root/'closure-trace-cell1-board.log').read_text()
a1=(root/'closure-trace-cell1-android.log').read_text()
assert b1.index('180AA1A78D73B81949470301E0585224') < b1.index('180ACDA89C75B819494703014E274164') < b1.index('CONTACT_ESTABLISHED')
assert a1.index('AP RX hex=180ACDA89C75B819494703014E274164') < a1.index('MACHINE event=3')
assert 'BLE RX deferred until candidate ready' in (root/'closure-trace-cell2-board.log').read_text()
print(json.dumps({'cells':results,'release_approved':False,'note':'Audio collision timing and third-party traffic timing require the accompanying audit; this verifier does not infer them.'},indent=2))
