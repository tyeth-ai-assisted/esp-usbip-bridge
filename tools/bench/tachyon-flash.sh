#!/bin/bash
# Flash an S31 bridge build through the bench host that holds its USB-UART
# (the Particle Tachyon): upload the images over SSH, run esptool there.
#
#   tools/bench/tachyon-flash.sh [build-dir] [port]
#
# - build-dir defaults to build-s31-function-coreboard-1, port to /dev/ttyUSB0
#   (CP2102N; the S31's USB-Serial/JTAG shows up as /dev/ttyACM0).
# - Refuses to flash while anything holds the port: a console capture left
#   running made esptool fail half way and left a broken image.
# - 460800 baud: the Tachyon's USB hub drops out when its USB-PD supply
#   renegotiates, and 921600 lost the chip mid-write after such a dropout.
# - The Tachyon's LAN goes through the same hub, so Tailscale is tried first.
set -u
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
B="${1:-$HERE/build-s31-function-coreboard-1}"
PORT="${2:-/dev/ttyUSB0}"
SSH="${SSH:-/c/Windows/System32/OpenSSH/ssh.exe}"
OPTS="-o BatchMode=yes -o ConnectTimeout=8"
HOSTS="${TACHYON_HOSTS:-100.101.245.66 192.168.1.169 192.168.1.145}"

run() {  # run <cmd> [stdin-file]: try every host, 3 rounds
  for round in 1 2 3; do
    for h in $HOSTS; do
      if [ -n "${2:-}" ]; then
        $SSH $OPTS particle@$h "$1" < "$2" && return 0
      else
        $SSH $OPTS particle@$h "$1" && return 0
      fi
      echo "[ssh via $h failed]" >&2
    done
  done
  return 1
}

[ -f "$B/flash_args" ] || { echo "no build in $B" >&2; exit 1; }
run "if sudo -n fuser $PORT >/dev/null 2>&1; then echo '$PORT BUSY:'; sudo -n fuser -v $PORT; exit 1; fi" \
  || { echo "FLASH ABORTED: $PORT busy or host unreachable"; exit 1; }
T=$(mktemp)
(cd "$B" && tar cf - flash_args bootloader/bootloader.bin partition_table/partition-table.bin esp32p4_usbip_bridge.bin) > "$T"
run "mkdir -p ~/s31-flash && cd ~/s31-flash && tar xf -" "$T" || { rm -f "$T"; echo "FLASH ABORTED: upload failed"; exit 1; }
rm -f "$T"
run "cd ~/s31-flash && P=\$(python3 -c 'import esptool,os;print(os.path.dirname(os.path.dirname(esptool.__file__)))'); sudo -n nohup env PYTHONPATH=\$P python3 -m esptool --chip esp32s31 -p $PORT -b 460800 write-flash @flash_args > flash.log 2>&1 & echo started" \
  || { echo "FLASH ABORTED: could not start esptool"; exit 1; }
out=""
for i in $(seq 1 60); do
  sleep 5
  out=$(run 'tr "\r" "\n" < ~/s31-flash/flash.log | grep -E "Hash of data|Hard resetting|rror" | tail -3; pgrep -f "[e]sptool --chip esp32s31" >/dev/null && echo RUNNING || echo DONE' 2>/dev/null) || continue
  echo "$out" | grep -q DONE && break
done
if echo "$out" | grep -q "Hard resetting" && [ "$(echo "$out" | grep -c 'Hash of data verified')" -ge 1 ]; then
  echo "FLASH OK"
else
  echo "FLASH FAILED"; run 'tail -8 ~/s31-flash/flash.log'; exit 1
fi
