#!/bin/bash
# Flash a blank or boot-looping ESP32 USB-Serial/JTAG DUT (303a:1001) over
# USB/IP, then optionally drop a UF2 on the UF2 bootloader it comes up with:
#   1. catch the ROM loader (re-attach whenever the bridge reports a new USB
#      address for the busid, esptool --before usb-reset), as the catch script;
#   2. esptool write-flash <offset> <image> and a hard reset;
#   3. with a UF2 given: wait for the busid to re-enumerate as something other
#      than 303a:1001 (the UF2 bootloader), attach, mount its drive, copy the
#      UF2, wait for the application to enumerate, attach and show its console.
#
#   esp-usbjtag-flash.sh <busid> <serial> <image.bin> [offset] [file.uf2] [seconds]
# Needs esptool v5 (dash syntax): ESPTOOL=/root/esptool5/bin/esptool by default.
. "$(dirname "$0")/lib.sh"
BUSID="${1:?busid}"; SERIAL="${2:?USB serial, e.g. F4:12:FA:59:5D:B0}"; IMAGE="${3:?image.bin}"
OFFSET="${4:-0x0}"; UF2="${5:-}"; LIMIT="${6:-90}"
ESPTOOL="${ESPTOOL:-/root/esptool5/bin/esptool}"
DEV="/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_${SERIAL}-if00"
W=$(mktemp -d)
cleanup_vhci

addr_of() { curl -s -m1 "http://$BRIDGE/api/usb/devices/$BUSID" | python3 -c 'import json,sys
try: print(json.load(sys.stdin)["address"])
except Exception: pass'; }

# --- 1. catch the ROM loader ---------------------------------------------
(
  last=""; end=$((SECONDS + LIMIT))
  while [ $SECONDS -lt $end ] && [ ! -e "$W/caught" ]; do
    a=$(addr_of)
    if [ -n "$a" ] && [ "$a" != "$last" ]; then
      cleanup_vhci
      usbip attach -r "$BRIDGE" -b "$BUSID" >/dev/null 2>&1 && last=$a
    fi
    sleep 0.05
  done
) &
ATT=$!
out=""; end=$((SECONDS + LIMIT)); t0=$SECONDS
while [ $SECONDS -lt $end ]; do
  [ -e "$DEV" ] || { sleep 0.02; continue; }
  for before in usb-reset no-reset; do
    out=$(timeout 8 "$ESPTOOL" -p "$DEV" --connect-attempts 1 --before $before --after no-reset chip-id 2>&1)
    echo "$out" | grep -q "Chip type" && break 2
    [ -e "$DEV" ] || break
  done
done
touch "$W/caught"; wait $ATT
echo "$out" | grep -q "Chip type" || { echo "NOT CAUGHT in $LIMIT s"; exit 1; }
echo "caught ROM loader in $((SECONDS - t0)) s: $(echo "$out" | grep "Chip type")"

# --- 2. write the image (image "-" = only reset out of the ROM loader) ------
if [ "$IMAGE" != "-" ]; then
  t0=$SECONDS
  wout=$("$ESPTOOL" -p "$DEV" --before no-reset --after no-reset write-flash "$OFFSET" "$IMAGE" 2>&1)
  echo "$wout" | grep -E "Wrote|Hash|rror" | tail -4
  echo "$wout" | grep -q "Hash of data verified" || { echo "WRITE FAILED:"; echo "$wout" | tail -15; exit 1; }
  echo "write-flash done in $((SECONDS - t0)) s"
fi
# esptool's RTS-style hard reset leaves this ROM in download mode; a watchdog
# reset boots the new image.
"$ESPTOOL" -p "$DEV" --before no-reset --after watchdog-reset chip-id 2>&1 | tail -1
cleanup_vhci
[ -z "$UF2" ] && exit 0

# --- 3. UF2 bootloader: copy the UF2 ---------------------------------------
echo "waiting for $BUSID to enumerate as the UF2 bootloader..."
for _ in $(seq 1 30); do
  v=$(bridge_dev "$BUSID"); [ -n "$v" ] && [ "$v" != "303a:1001" ] && break; sleep 1
done
v=$(bridge_dev "$BUSID"); echo "bridge now sees $BUSID as ${v:-nothing}"
[ -n "$v" ] && [ "$v" != "303a:1001" ] || { echo "no UF2 bootloader came up"; exit 1; }
attach "$BUSID" >/dev/null; sleep 8
dev=""
for d in $(lsblk -dno NAME,TRAN | awk '$2=="usb"{print $1}'); do
  for p in /dev/"${d}"1 /dev/"$d"; do [ -b "$p" ] && blkid "$p" | grep -q vfat && { dev="$p"; break 2; }; done
done
[ -n "$dev" ] || { echo "no UF2 drive"; exit 1; }
mkdir -p /mnt/uf2 && mount "$dev" /mnt/uf2 || { echo "mount failed"; exit 1; }
echo "UF2 drive $dev: $(head -c 200 /mnt/uf2/INFO_UF2.TXT 2>/dev/null | tr '\n' ' ')"
t0=$SECONDS
cp "$UF2" /mnt/uf2/ 2>/dev/null; sync 2>/dev/null; umount -l /mnt/uf2 2>/dev/null
echo "UF2 copied ($(stat -c %s "$UF2") bytes) in $((SECONDS - t0)) s"
sleep 5; cleanup_vhci
echo "waiting for the application to enumerate..."
for _ in $(seq 1 40); do
  v2=$(bridge_dev "$BUSID"); [ -n "$v2" ] && [ "$v2" != "$v" ] && [ "$v2" != "303a:1001" ] && break; sleep 1
done
v2=$(bridge_dev "$BUSID"); echo "bridge now sees $BUSID as ${v2:-nothing}"
lb=$(attach "$BUSID"); tty=$(tty_of "$lb")
[ -n "$tty" ] && { echo "console /dev/$tty:"; python3 - "/dev/$tty" <<'PY'
import serial, sys, time
s = serial.Serial(sys.argv[1], 115200, timeout=0.5); end = time.monotonic() + 12; got = b""
while time.monotonic() < end:
    got += s.read(4096)
print(got.decode(errors="replace")[-1500:])
s.close()
PY
}
cleanup_vhci
