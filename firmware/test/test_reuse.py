#!/usr/bin/env python3
"""Repro for stale bytes on socket reuse: page GET, WebSocket handshake, close,
repeat. Prints counts by outcome. Usage: test_reuse.py [N] [--page]
A healthy box shows only page_ok and ok."""
import base64,hashlib,os,socket,time,urllib.request,collections,sys
H=os.environ.get('ZRP6_HOST', '10.0.0.77'); N=int(sys.argv[1]) if len(sys.argv)>1 else 40
res=collections.Counter(); t0=time.time()
for i in range(N):
    if '--page' in sys.argv:
        try:
            b=urllib.request.urlopen(f'http://{H}/',timeout=10).read(); res['page_ok' if len(b)>20000 else 'page_short']+=1
        except Exception as e: res['page_'+type(e).__name__]+=1
    key=base64.b64encode(os.urandom(16))
    s=socket.create_connection((H,80),5); s.settimeout(3)
    s.sendall(b'GET / HTTP/1.1\r\nHost: z\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: '+key+b'\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: mqtt\r\n\r\n')
    st=time.time()
    try:
        r=b''
        while b'\r\n\r\n' not in r:
            c=s.recv(1024)
            if not c: break
            r+=c
        want=base64.b64encode(hashlib.sha1(key+b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest())
        k='ok' if want in r else ('closed' if not r else 'badaccept')
        if k=='ok': res['ms_max']=max(res['ms_max'],int((time.time()-st)*1000))
    except socket.timeout: k='timeout'
    except ConnectionResetError: k='reset'
    res[k]+=1; s.close(); time.sleep(0.3)
print(dict(res), f'{time.time()-t0:.0f}s')
