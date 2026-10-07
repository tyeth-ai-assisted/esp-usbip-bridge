"""Reboot the bridge N times with a 5 s enumeration timeout and report every
enumeration failure with its diagnosis and outcome, plus whether all devices
ended up enumerated (no port left disabled)."""
import collections
import json
import re
import sys
import threading
import time
import urllib.request

import serial

B = 'http://192.168.1.147'
RUNS = int(sys.argv[1]) if len(sys.argv) > 1 else 20
SETTLE = 22          # 5 s timeout + 1 s retry back-off + enumeration of everything
EXPECTED = {'1-1.2', '1-1.3', '1-1.4', '1-1.1.1', '1-1.1.2', '1-1.1.3'}


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


def reboot_and_log(seconds):
    log = []
    stop = threading.Event()

    def rd():
        with serial.Serial('COM21', 115200, timeout=0.2) as s:
            s.dtr = False
            s.rts = True
            time.sleep(0.1)
            s.rts = False
            while not stop.is_set():
                d = s.read(4096)
                if d:
                    log.append(d)

    th = threading.Thread(target=rd, daemon=True)
    th.start()
    time.sleep(seconds)
    return log, stop, th


print('set global timeout 5000:', req('POST', '/api/settings/enum_timeout', {'timeout_ms': 5000})[0])
diag = collections.Counter()
outcomes = collections.Counter()
missing_runs = 0
crashes = 0
timeouts = []
for i in range(RUNS):
    log, stop, th = reboot_and_log(SETTLE)
    _, devs = req('GET', '/api/usb/devices')
    _, ev = req('GET', '/api/usb/enum_events')
    stop.set()
    th.join(2)
    txt = re.sub(r'\x1b\[[0-9;]*m', '', b''.join(log).decode('utf-8', 'replace'))
    crash = bool(re.search(r'Guru|abort\(\)|assert failed', txt))
    crashes += crash
    present = {d['busid'] for d in (devs or {}).get('devices', []) if not d['virtual']}
    missing = EXPECTED - present
    fails = [e for e in (ev or {}).get('events', []) if e['event'] == 'failed']
    for f in fails:
        diag[(f['reason'], f.get('transfer_status'))] += 1
        outcomes[f['outcome']] += 1
        if f['reason'] == 'timeout':
            timeouts.append((i, f['path'], f['stage'], f['outcome'], f.get('outcome_after_ms')))
    # A boot-looping DUT may be mid-reboot at the instant we sample
    flappy = missing <= {'1-1.1.2'}
    if missing and not flappy:
        missing_runs += 1
    print(f'{i:2d} devices={len(present)} missing={sorted(missing) or "-"} failures={len(fails)} '
          f'{"CRASH" if crash else ""}', flush=True)
    for f in fails:
        if f['path'] != '1-1.1.2' or f['reason'] == 'timeout':
            print(f"     {f['path']:8s} {f['reason']:14s} stage={f['stage']} xfer={f.get('transfer_status')} "
                  f"elapsed={f['elapsed_ms']}ms -> {f['outcome']} {f.get('outcome_after_ms', '')}", flush=True)

print('\nfailure reasons:', dict(diag))
print('outcomes:', dict(outcomes))
print('timeouts (run, path, stage, outcome, after_ms):', timeouts)
print(f'runs with a non-flapping device missing: {missing_runs}/{RUNS}; crashes: {crashes}')
_, ev = req('GET', '/api/usb/enum_events')
sample = [e for e in ev['events'] if e['event'] == 'failed'][:3]
print('sample failure records:', json.dumps(sample, indent=1))
