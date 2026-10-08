#!/bin/bash
# Catch a boot-looping ESP32 USB-Serial/JTAG DUT (303a:1001) in its ROM loader
# over USB/IP and read what it is, without writing anything:
#   - attacher: re-attach as soon as the bridge reports a new USB address for
#     the busid (the bridge keeps the old USB/IP connection open, so the Linux
#     side goes stale on each reboot);
#   - prober: esptool on the stable /dev/serial/by-id path, in a tight loop.
#     It tries --before usb-reset, then --before no-reset (the reset may have
#     put the chip in the ROM loader without re-enumerating it).
# On success the chip stays in the ROM loader; the partition table and app
# descriptor are read, then --after watchdog-reset restarts it normally.
#
#   esp-usbjtag-catch.sh <busid> <serial> [seconds]
# Needs esptool v5 (dash syntax): ESPTOOL=/root/esptool5/bin/esptool by default.
. "$(dirname "$0")/lib.sh"
BUSID="${1:?busid}"; SERIAL="${2:?USB serial, e.g. F4:12:FA:59:5D:B0}"; LIMIT="${3:-90}"
ESPTOOL="${ESPTOOL:-/root/esptool5/bin/esptool}"
DEV="/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_${SERIAL}-if00"
W=$(mktemp -d)
cleanup_vhci

addr_of() { curl -s -m1 "http://$BRIDGE/api/usb/devices/$BUSID" | python3 -c 'import json,sys
try: print(json.load(sys.stdin)["address"])
except Exception: pass'; }

(
  last=""; n=0; end=$((SECONDS + LIMIT))
  while [ $SECONDS -lt $end ] && [ ! -e "$W/caught" ]; do
    a=$(addr_of)
    if [ -n "$a" ] && [ "$a" != "$last" ]; then
      cleanup_vhci
      usbip attach -r "$BRIDGE" -b "$BUSID" >/dev/null 2>&1 && { last=$a; n=$((n + 1)); }
    fi
    sleep 0.05
  done
  echo "$n" > "$W/attaches"
) &
ATT=$!

tries=0; out=""; end=$((SECONDS + LIMIT))
while [ $SECONDS -lt $end ]; do
  [ -e "$DEV" ] || { sleep 0.02; continue; }
  tries=$((tries + 1))
  for before in usb-reset no-reset; do
    out=$(timeout 8 "$ESPTOOL" -p "$DEV" --connect-attempts 1 --before $before --after no-reset flash-id 2>&1)
    echo "$out" | grep -q "Chip type" && break 2
    echo "$before: $(echo "$out" | grep -E "rror|Errno|Failed" | tail -1 | cut -c1-90)" >> "$W/errors"
    [ -e "$DEV" ] || break
  done
done
touch "$W/caught"; wait $ATT
echo "attaches: $(cat "$W/attaches"), esptool rounds: $tries in $SECONDS s"
if ! echo "$out" | grep -q "Chip type"; then
  echo "NOT CAUGHT; most common errors:"; sort "$W/errors" | uniq -c | sort -rn | head -6
  exit 1
fi
echo "CAUGHT"
echo "$out" | grep -E "Chip type|Features|Crystal|MAC|Manufacturer|Device|flash size"
"$ESPTOOL" -p "$DEV" --before no-reset --after no-reset read-flash 0x8000 0xc00 "$W/pt.bin" >/dev/null 2>&1
"$ESPTOOL" -p "$DEV" --before no-reset --after no-reset read-flash 0x0 0x40 "$W/boot.bin" >/dev/null 2>&1
python3 - "$W/pt.bin" "$W/app_off" "$W/boot.bin" <<'PY'
import struct, sys
d = open(sys.argv[1], "rb").read(); app = None
for i in range(0, len(d), 32):
    e = d[i:i + 32]
    if e[:2] != b"\xaa\x50":
        break
    typ, sub = e[2], e[3]; off, size = struct.unpack("<II", e[4:12])
    name = e[12:28].split(b"\x00")[0].decode(errors="replace")
    print(f"  {name:16s} type={typ} sub=0x{sub:02x} off=0x{off:x} size=0x{size:x}")
    if typ == 0 and app is None:
        app = off
if app is None:
    # No partition table: show what is there (0xff = erased flash, so the ROM
    # finds no bootloader and its watchdog resets the chip over and over)
    b = open(sys.argv[3], "rb").read() if len(sys.argv) > 3 else b""
    print("  no partition table at 0x8000:", d[:16].hex(), "...", "erased" if d and set(d) == {0xff} else "data")
    print("  bootloader region 0x0  :", b[:16].hex(), "...", "erased" if b and set(b) == {0xff} else ("ESP image magic" if b[:1] == b"\xe9" else "data"))
open(sys.argv[2], "w").write(hex(app or 0x10000))
PY
"$ESPTOOL" -p "$DEV" --before no-reset --after no-reset read-flash "$(cat "$W/app_off")" 0x100 "$W/app.bin" >/dev/null 2>&1
python3 - "$W/app.bin" <<'PY'
import struct, sys
desc = open(sys.argv[1], "rb").read()[0x20:0x120]
if struct.unpack("<I", desc[:4])[0] == 0xABCD5432:
    s = lambda b: b.split(b"\0")[0].decode(errors="replace")
    print("  app     :", s(desc[48:80]), s(desc[16:48]))
    print("  built   :", s(desc[96:112]), s(desc[80:96]), "IDF", s(desc[112:144]))
else:
    print("  no app descriptor at the app partition")
PY
"$ESPTOOL" -p "$DEV" --before no-reset --after watchdog-reset chip-id 2>&1 | tail -1
cleanup_vhci
