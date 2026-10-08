#!/bin/bash
# Helpers for the Linux USB/IP client tests (run as root on the client, e.g.
#   MSYS_NO_PATHCONV=1 wsl -d Ubuntu -u root -- bash tools/bench/usbip-client/<test>.sh
# from a Windows checkout). BRIDGE can be overridden in the environment.
BRIDGE="${BRIDGE:-192.168.1.147}"
VHCI=/sys/devices/platform/vhci_hcd.0
export PATH=/usr/local/sbin:/usr/local/bin:$PATH

# The client kernel (e.g. after a WSL restart) may not have these loaded yet.
for m in vhci-hcd cdc-acm cp210x usb-storage; do modprobe "$m" 2>/dev/null; done

# Detach every used vhci port through sysfs (works even when libusbip can't).
cleanup_vhci() {
  awk 'NR>1 && $3!="004"{print $2+0}' $VHCI/status | while read -r p; do
    echo "$p" > $VHCI/detach 2>/dev/null
  done
}

# Put a marker in the kernel log; dmesg_since <tag> prints what follows it.
mark() { echo "TESTMARK $1" > /dev/kmsg; }
dmesg_since() { dmesg | awk -v t="TESTMARK $1" 'index($0,t){f=1;next} f'; }

# bridge_dev <busid> -> "vid:pid" (empty when absent)
bridge_dev() {
  curl -s -m 5 "http://$BRIDGE/api/usb/devices/$1" | python3 -c 'import json,sys
try:
    d = json.load(sys.stdin); print("%s:%s" % (d["vid"], d["pid"]))
except Exception:
    pass'
}

# wait_bridge_dev <busid> <vid:pid> <seconds>
wait_bridge_dev() {
  for _ in $(seq 1 "$3"); do
    [ "$(bridge_dev "$1")" = "$2" ] && return 0
    sleep 1
  done
  return 1
}

# attach <busid>: prints the local busid of the new attachment (e.g. 1-1)
attach() {
  usbip attach -r "$BRIDGE" -b "$1" >/dev/null || return 1
  sleep 2
  awk 'NR>1 && $3!="004"{print $7}' $VHCI/status | tail -1
}

# tty_of <local busid> [ttyACM|ttyUSB]
tty_of() {
  local kind="${2:-ttyACM}"
  ls -d /sys/bus/usb/devices/"$1":*/tty/"$kind"* /sys/bus/usb/devices/"$1":*/"$kind"* 2>/dev/null \
    | head -1 | xargs -r basename
}

# usb_vfat_dev: the first vfat filesystem on a USB block device
usb_vfat_dev() {
  local d p
  for d in $(lsblk -dno NAME,TRAN | awk '$2=="usb"{print $1}'); do
    for p in /dev/"${d}"1 /dev/"$d"; do
      [ -b "$p" ] && blkid "$p" | grep -q vfat && { echo "$p"; return 0; }
    done
  done
  return 1
}

# 1200-baud touch on a CDC ACM tty (RP2040/CircuitPython -> BOOTSEL)
touch_1200() { python3 -c "import serial,time; s=serial.Serial('/dev/$1',1200); time.sleep(0.3); s.close()"; }
