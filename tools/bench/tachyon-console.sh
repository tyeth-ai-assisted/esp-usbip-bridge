#!/bin/bash
# Capture the S31 bridge console (its CP2102N on the Tachyon) for N seconds
# and print the last lines.
#
#   tools/bench/tachyon-console.sh [seconds] [lines] [port]
#
# A plain serial.Serial() open: DTR/RTS are not touched afterwards (setting
# dtr=False while RTS is asserted resets the bridge). Each capture writes its
# own ~/s31-flash/console-HHMMSS.log, so overlapping captures don't clobber
# each other. Stop any capture before flashing (tachyon-flash.sh checks).
set -u
SECS="${1:-20}"; LINES="${2:-80}"; PORT="${3:-/dev/ttyUSB0}"
SSH="${SSH:-/c/Windows/System32/OpenSSH/ssh.exe}"
OPTS="-o BatchMode=yes -o ConnectTimeout=8"
HOSTS="${TACHYON_HOSTS:-100.101.245.66 192.168.1.169 192.168.1.145}"
LOG="console-$(date +%H%M%S).log"
CMD="sudo -n python3 -c 'import serial,time
s=serial.Serial(\"$PORT\",115200,timeout=0.5)
end=time.time()+$SECS
f=open(\"/home/particle/s31-flash/$LOG\",\"wb\")
while time.time()<end:
  d=s.read(4096)
  if d: f.write(d); f.flush()
s.close()'; tr -d '\r' < ~/s31-flash/$LOG | tail -$LINES"
for round in 1 2 3; do
  for h in $HOSTS; do
    $SSH $OPTS particle@$h "mkdir -p ~/s31-flash; $CMD" && exit 0
    echo "[ssh via $h failed]" >&2
  done
done
exit 1
