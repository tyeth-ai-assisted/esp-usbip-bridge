"""1) Per-port verdicts after a normal boot (Pico on 1-1.4 in particular).
2) Force enumeration timeouts on 1-1.2 with a 1 ms override, check the
   transfer is aborted, the port retried with back-off (never left disabled),
   then remove the override and check it recovers by itself."""
import json
import re
import threading
import time
import urllib.request

import serial

B = 'http://192.168.1.147'
log = []
stop = threading.Event()


def reader():
    with serial.Serial('COM21', 115200, timeout=0.2) as s:
        while not stop.is_set():
            d = s.read(4096)
            if d:
                log.append(d)


def req(m, p, body=None):
    data = json.dumps(body).encode() if body is not None else None
    r = urllib.request.Request(B + p, data=data, method=m, headers={'Content-Type': 'application/json'})
    for _ in range(5):
        try:
            with urllib.request.urlopen(r, timeout=10) as f:
                return f.status, json.load(f)
        except urllib.error.HTTPError as e:
            return e.code, json.load(e)
        except OSError:
            time.sleep(1)
    return None, None


threading.Thread(target=reader, daemon=True).start()
time.sleep(40)   # let the boot-loopers and the Pico show their pattern
ev = req('GET', '/api/usb/enum_events')[1]
print('window', ev['window_ms'], 'ms; per-port verdicts:')
for path, v in sorted(ev['ports'].items()):
    print(f'  {path:8s} {v}')

print('\n1 ms override on 1-1.2:', req('POST', '/api/settings/enum_timeout', {'path': '1-1.2', 'timeout_ms': 1})[0])
print('cycle 1-1.2:', req('POST', '/api/ports/1-1.2/cycle', {'force': True, 'off_ms': 300})[0])
time.sleep(9)
st = [p for h in req('GET', '/api/usb/hubs')[1]['hubs'] for p in h['ports'] if p['path'] == '1-1.2'][0]
print('  port 1-1.2 now:', {k: st[k] for k in ('power', 'connected', 'enabled', 'device')})
print('  present:', req('GET', '/api/usb/devices/1-1.2')[0])
ev = req('GET', '/api/usb/enum_events')[1]
for e in [e for e in ev['events'] if e['path'] == '1-1.2'][:6]:
    print('   ', e['event'], e.get('reason', ''), e.get('stage', ''), e.get('elapsed_ms', ''), e.get('outcome', ''))
print('  verdict:', ev['ports'].get('1-1.2'))

t0 = time.time()
print('\nremove override:', req('POST', '/api/settings/enum_timeout', {'path': '1-1.2', 'timeout_ms': None})[0])
while time.time() - t0 < 40 and req('GET', '/api/usb/devices/1-1.2')[0] != 200:
    time.sleep(0.5)
print(f'  1-1.2 back after {time.time() - t0:.1f} s without any manual action:', req('GET', '/api/usb/devices/1-1.2')[0])
print('  verdict:', req('GET', '/api/usb/enum_events')[1]['ports'].get('1-1.2'))
stop.set()
time.sleep(0.5)
txt = re.sub(r'\x1b\[[0-9;]*m', '', b''.join(log).decode('utf-8', 'replace'))
for line in txt.splitlines():
    if re.search(r'ENUM: Device|retry|retrying|Guru|abort\(\)|assert', line):
        print('  LOG', line)
print('devices:', sorted(d['busid'] for d in req('GET', '/api/usb/devices')[1]['devices']))
