"""Exercise enumeration timeout settings, overrides and the timeout path."""
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
    try:
        with serial.Serial('COM21', 115200, timeout=0.2) as s:
            while not stop.is_set():
                d = s.read(4096)
                if d:
                    log.append(d)
    except serial.SerialException as e:
        print('serial:', e)


def req(m, p, body=None):
    data = json.dumps(body).encode() if body is not None else None
    r = urllib.request.Request(B + p, data=data, method=m, headers={'Content-Type': 'application/json'})
    for attempt in range(3):
        try:
            with urllib.request.urlopen(r, timeout=10) as f:
                return f.status, json.load(f)
        except urllib.error.HTTPError as e:
            return e.code, json.load(e)
        except OSError as e:
            print(f'  ({m} {p}: {e}; retrying)')
            time.sleep(0.5)
    return 'EXC', None


def eff():
    out = {}
    for h in req('GET', '/api/usb/hubs')[1]['hubs']:
        out[h['path']] = (h['enum_timeout_ms'], '*' if h['enum_timeout_override'] else '')
        for p in h['ports']:
            out[p['path']] = (p['enum_timeout_ms'], '*' if p['enum_timeout_override'] else '')
    return out


th = threading.Thread(target=reader, daemon=True)
th.start()
try:
    print('global 1500', req('POST', '/api/settings/enum_timeout', {'timeout_ms': 1500})[0])
    print('hub 1-1.1 -> 3000', req('POST', '/api/settings/enum_timeout', {'path': '1-1.1', 'timeout_ms': 3000})[0])
    print('port 1-1.1.2 -> 0', req('POST', '/api/settings/enum_timeout', {'path': '1-1.1.2', 'timeout_ms': 0})[0])
    print('bad path', req('POST', '/api/settings/enum_timeout', {'path': '9-9', 'timeout_ms': 5}))
    e = eff()
    print('effective (* = override):', {k: e[k] for k in sorted(e)})

    print('port 1-1.4 -> 1 ms', req('POST', '/api/settings/enum_timeout', {'path': '1-1.4', 'timeout_ms': 1})[0])
    print('cycle 1-1.4', req('POST', '/api/ports/1-1.4/cycle', {'force': True, 'off_ms': 500})[0])
    time.sleep(4)
    print('  1-1.4 present:', req('GET', '/api/usb/devices/1-1.4')[0],
          '| devices:', sorted(d['busid'] for d in req('GET', '/api/usb/devices')[1]['devices']))
    print('remove 1-1.4 override', req('POST', '/api/settings/enum_timeout', {'path': '1-1.4', 'timeout_ms': None})[0])
    print('cycle 1-1.4', req('POST', '/api/ports/1-1.4/cycle', {'force': True, 'off_ms': 500})[0])
    time.sleep(4)
    print('  1-1.4 present:', req('GET', '/api/usb/devices/1-1.4')[0])
finally:
    stop.set()
    th.join(2)

txt = re.sub(r'\x1b\[[0-9;]*m', '', b''.join(log).decode('utf-8', 'replace'))
for line in txt.splitlines():
    if re.search(r'ENUM|did not answer|disabl|USB HOST|hub_ctl|Guru|abort|assert', line):
        print('  LOG', line)
