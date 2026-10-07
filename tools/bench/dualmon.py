"""Reset the bridge N times while logging both consoles (CP2102N UART on COM21
and USB-Serial/JTAG on COM31), then report boot strap value, USB tree and
crashes per boot. Logs of interesting boots are kept in scratchpad/runs/."""
import json
import os
import re
import sys
import threading
import time
import urllib.request

import serial

UART = 'COM21'
JTAG = 'COM31'
BASE = 'http://192.168.1.147'
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'runs', time.strftime('%H%M%S'))
RUNS = int(sys.argv[1]) if len(sys.argv) > 1 else 10
RESET = sys.argv[2] if len(sys.argv) > 2 else 'uart'   # uart | jtag
WAIT = 12
os.makedirs(OUT, exist_ok=True)
ANSI = re.compile(r'\x1b\[[0-9;]*m')


def reader(port, sink, stop):
    """Keep (re)opening `port` and append everything read to sink."""
    while not stop.is_set():
        try:
            with serial.Serial(port, 115200, timeout=0.2) as s:
                while not stop.is_set():
                    d = s.read(4096)
                    if d:
                        sink.append((time.time(), d))
        except (serial.SerialException, OSError):
            time.sleep(0.1)


def reset(method):
    if method == 'uart':
        with serial.Serial(UART, 115200, timeout=0.2) as s:
            s.dtr = False
            s.rts = True
            time.sleep(0.1)
            s.rts = False
    else:
        # USB-Serial/JTAG: RTS asserts chip reset, DTR low keeps GPIO boot high
        with serial.Serial(JTAG, 115200, timeout=0.2) as s:
            s.dtr = False
            s.rts = True
            time.sleep(0.1)
            s.rts = False


def text(chunks):
    return ANSI.sub('', b''.join(d for _, d in chunks).decode('utf-8', 'replace'))


def usb_tree():
    try:
        hubs = json.load(urllib.request.urlopen(BASE + '/api/usb/hubs', timeout=5))['hubs']
        devs = json.load(urllib.request.urlopen(BASE + '/api/usb/devices', timeout=5))['devices']
    except Exception as e:  # noqa: BLE001
        return None, None, str(e)
    on = sum(1 for h in hubs for p in h['ports'] if p['power'] == 'on')
    real = sorted(d['busid'] for d in devs if not d['virtual'])
    return (len(hubs), on), real, None


summary = []
for i in range(RUNS):
    uart_log, jtag_log = [], []
    stop = threading.Event()
    tu = threading.Thread(target=reader, args=(UART, uart_log, stop), daemon=True)
    tj = threading.Thread(target=reader, args=(JTAG, jtag_log, stop), daemon=True)
    # Reset first (needs the port), then start monitoring immediately
    reset(RESET)
    tu.start()
    tj.start()
    time.sleep(WAIT)
    hub_state, devs, err = usb_tree()
    if devs is not None and len(devs) < 5:
        # Stalled: dump the USB host state to both consoles while still logging
        try:
            urllib.request.urlopen(urllib.request.Request(BASE + '/api/usb/debug', method='POST'),
                                   timeout=5).read()
        except Exception as e:  # noqa: BLE001
            print('debug dump failed:', e)
        time.sleep(1.5)
    stop.set()
    tu.join(1)
    tj.join(1)

    ut, jt = text(uart_log), text(jtag_log)
    m = re.search(r'rst:(0x[0-9a-f]+) \(([A-Z_0-9]+)\),boot:(0x[0-9a-f]+)', ut) or \
        re.search(r'rst:(0x[0-9a-f]+) \(([A-Z_0-9]+)\),boot:(0x[0-9a-f]+)', jt)
    boot = m.group(3) if m else '?'
    crash = bool(re.search(r'Guru|abort\(\)|assert failed|panic', ut + jt))
    exported = len(re.findall(r'Exporting USB device', ut))
    ndev = len(devs) if devs is not None else -1
    status = 'OK' if ndev >= 5 and not crash else ('CRASH' if crash else 'STALL' if ndev >= 0 else 'NOAPI')
    summary.append((i, boot, status, hub_state, ndev, exported, len(jt)))
    print(f'{i:2d} boot={boot} {status:5s} hubs/ports_on={hub_state} devices={ndev} '
          f'exported_logs={exported} jtag_bytes={len(jt)} {err or ""}', flush=True)
    timeout_fired = 'did not answer' in ut + jt
    if timeout_fired:
        print(f'   enumeration timeout fired on boot {i}:',
              [l for l in (ut + jt).splitlines() if 'did not answer' in l][:1], flush=True)
    if status != 'OK' or i == 0 or timeout_fired:
        with open(os.path.join(OUT, f'run{i:02d}_{status}_uart.log'), 'w', encoding='utf-8') as f:
            f.write(ut)
        with open(os.path.join(OUT, f'run{i:02d}_{status}_jtag.log'), 'w', encoding='utf-8') as f:
            f.write(jt)

bad = [s for s in summary if s[2] != 'OK']
print(f'\n{RUNS - len(bad)}/{RUNS} OK; boot values: '
      f'{sorted(set(s[1] for s in summary))}; failures by boot value: '
      f'{[(s[0], s[1], s[2]) for s in bad]}')
