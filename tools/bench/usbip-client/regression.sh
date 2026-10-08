#!/bin/bash
# USB/IP client regression for the S31 bench (usb-storage left bound):
#   bootsel  RP2040 (RP2040_BUSID, CircuitPython) -> 1200-baud touch -> BOOTSEL:
#            no usb-storage reset loop, RPI-RP2 detected, picotool info, picotool reboot
#   uf2      picotool save -a of the whole flash, then copy it back as a UF2 (4 MiB write)
#   esptool  esptool --no-stub flash_id twice on one attach (ESPTOOL_BUSID), no "cannot find a urb"
#   msc      read/write 64 KiB on each mass-storage drive in MSC_BUSIDS
#   open     a pyserial open of each idle CDC port in OPEN_BUSIDS takes under 1 s (cdc-acm's 16
#            pending reads must not delay its control requests; it used to take 9-18 s)
#   idle     the quiet CDC console IDLE_BUSID, left open and idle for IDLE_SECS with cdc-acm's 16
#            reads pending, has no failed URB (no -110 timeouts, no -104 sibling cancels) and
#            still answers Ctrl-C/Enter, twice
#   timeout  a 20 s libusb bulk read on IDLE_BUSID's idle CDC data endpoint is never timed out by
#            the bridge (no URB status -110); libusb's own timeout unlinks it (LIBUSB_ERROR_TIMEOUT,
#            -7) and the console still answers afterwards
#
#   regression.sh [test ...]     (default: all, in the order above)
. "$(dirname "$0")/lib.sh"
RP2040_BUSID="${RP2040_BUSID:-1-1.4}"
ESPTOOL_BUSID="${ESPTOOL_BUSID:-1-1.3}"
MSC_BUSIDS="${MSC_BUSIDS:-1-1.1.3 1-1.2}"
OPEN_BUSIDS="${OPEN_BUSIDS:-1-1.4 1-1.1.1}"
IDLE_BUSID="${IDLE_BUSID:-1-1.1.1}"      # Feather S3 Reverse TFT: prints nothing unless poked
IDLE_SECS="${IDLE_SECS:-25}"
FAIL=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAIL=1; }

to_bootsel() {
  [ "$(bridge_dev "$RP2040_BUSID")" = "2e8a:0003" ] && return 0
  cleanup_vhci; sleep 1
  local lb tty; lb=$(attach "$RP2040_BUSID"); sleep 3
  tty=$(tty_of "$lb")
  [ -n "$tty" ] && touch_1200 "$tty" 2>/dev/null
  sleep 2; cleanup_vhci
  wait_bridge_dev "$RP2040_BUSID" 2e8a:0003 20
}

t_bootsel() {
  to_bootsel || { fail "bootsel: $RP2040_BUSID did not enter BOOTSEL ($(bridge_dev "$RP2040_BUSID"))"; return; }
  local tag=bootsel-$(date +%s); mark "$tag"
  attach "$RP2040_BUSID" >/dev/null; sleep 12
  local resets; resets=$(dmesg_since "$tag" | grep -c 'reset full-speed')
  [ "$resets" -eq 0 ] && pass "bootsel: no usb-storage resets" || fail "bootsel: $resets usb-storage resets"
  [ -n "$(timeout 30 blkid -L RPI-RP2)" ] && pass "bootsel: RPI-RP2 detected" || fail "bootsel: no RPI-RP2"
  timeout 30 picotool info -a 2>&1 | grep -q "type: *RP2040" && pass "bootsel: picotool info -a" || fail "bootsel: picotool info"
}

t_uf2() {
  to_bootsel || { fail "uf2: no BOOTSEL"; return; }
  cleanup_vhci; sleep 1; attach "$RP2040_BUSID" >/dev/null; sleep 8
  timeout 600 picotool save -a /root/rp2040_full.uf2 >/dev/null 2>&1 \
    && pass "uf2: picotool save -a ($(stat -c %s /root/rp2040_full.uf2) bytes)" || { fail "uf2: picotool save"; return; }
  timeout 30 picotool reboot >/dev/null 2>&1; sleep 2; cleanup_vhci
  wait_bridge_dev "$RP2040_BUSID" 2e8a:f00a 20 && pass "uf2: picotool reboot -> application" || fail "uf2: picotool reboot"
  to_bootsel || { fail "uf2: no BOOTSEL for the copy"; return; }
  cleanup_vhci; sleep 1; attach "$RP2040_BUSID" >/dev/null; sleep 8
  local dev; dev=$(timeout 30 blkid -L RPI-RP2)
  mkdir -p /mnt/rp2 && mount "$dev" /mnt/rp2 || { fail "uf2: mount RPI-RP2"; return; }
  local t0=$SECONDS
  cp /root/rp2040_full.uf2 /mnt/rp2/ 2>/dev/null; sync 2>/dev/null
  umount -l /mnt/rp2 2>/dev/null; sleep 5; cleanup_vhci
  wait_bridge_dev "$RP2040_BUSID" 2e8a:f00a 30 && pass "uf2: 4 MiB UF2 copy flashed ($((SECONDS - t0)) s)" || fail "uf2: board did not come back after the copy"
}

t_esptool() {
  cleanup_vhci; sleep 6
  local tag=esp-$(date +%s); mark "$tag"
  local lb tty n; lb=$(attach "$ESPTOOL_BUSID"); sleep 3; tty=$(tty_of "$lb" ttyUSB)
  for n in 1 2; do
    timeout 90 esptool --no-stub -p "/dev/$tty" flash_id 2>&1 | grep -q "Detected flash size" \
      && pass "esptool: flash_id run $n" || fail "esptool: flash_id run $n"
  done
  [ "$(dmesg_since "$tag" | grep -c 'cannot find a urb')" -eq 0 ] && pass "esptool: no 'cannot find a urb'" || fail "esptool: 'cannot find a urb' in dmesg"
}

t_msc() {
  local b
  for b in $MSC_BUSIDS; do
    cleanup_vhci; sleep 6
    local tag=msc-$(date +%s); mark "$tag"
    attach "$b" >/dev/null; sleep 15
    local dev; dev=$(usb_vfat_dev)
    [ -z "$dev" ] && { fail "msc $b: no vfat drive"; continue; }
    mkdir -p /mnt/msc && mount -o rw "$dev" /mnt/msc || { fail "msc $b: mount"; continue; }
    head -c 65536 /dev/urandom > /tmp/msc_rw.bin
    cp /tmp/msc_rw.bin /mnt/msc/hil_rw_test.bin && sync
    echo 3 > /proc/sys/vm/drop_caches
    cmp -s /tmp/msc_rw.bin /mnt/msc/hil_rw_test.bin && pass "msc $b: 64 KiB written and read back" || fail "msc $b: read-back mismatch"
    rm -f /mnt/msc/hil_rw_test.bin; sync; umount /mnt/msc
    local resets; resets=$(dmesg_since "$tag" | grep -c 'reset full-speed')
    [ "$resets" -eq 0 ] && pass "msc $b: no usb-storage resets" || fail "msc $b: $resets usb-storage resets"
  done
}

# The RP2040 must be running CircuitPython (not left in BOOTSEL by an earlier test).
rp2040_app() {
  if [ "$(bridge_dev "$RP2040_BUSID")" = "2e8a:0003" ]; then
    attach "$RP2040_BUSID" >/dev/null; sleep 5
    timeout 30 picotool reboot >/dev/null 2>&1; sleep 2; cleanup_vhci
  fi
  wait_bridge_dev "$RP2040_BUSID" 2e8a:f00a 20
}

t_open() {
  local b
  for b in $OPEN_BUSIDS; do
    cleanup_vhci; sleep 3
    [ "$b" = "$RP2040_BUSID" ] && { rp2040_app || { fail "open $b: not running its application"; continue; }; }
    local lb tty; lb=$(attach "$b"); tty=$(tty_of "$lb")
    [ -z "$tty" ] && { fail "open $b: no ttyACM"; continue; }
    local times worst; times=$(serial_open_times "$tty" 3 | tr '\n' ' ')
    worst=$(echo "$times" | tr ' ' '\n' | sort -n | tail -1)
    [ -n "$worst" ] && awk -v w="$worst" 'BEGIN{exit !(w < 1.0)}' \
      && pass "open $b: serial open times $times s" || fail "open $b: serial open times $times s (limit 1.0 s)"
  done
}

t_idle() {
  cleanup_vhci; sleep 3
  acm_debug_on
  local tag=idle-$(date +%s); mark "$tag"
  local lb tty; lb=$(attach "$IDLE_BUSID"); tty=$(tty_of "$lb")
  [ -z "$tty" ] && { fail "idle: no ttyACM on $IDLE_BUSID"; return; }
  local r1 r2 errs; r1=$(console_probe "$tty" "$IDLE_SECS"); r2=$(console_probe "$tty" 10)
  errs=$(acm_read_errors_since "$tag")
  [ "$errs" -eq 0 ] && pass "idle: no failed cdc-acm read in $IDLE_SECS s + 10 s with the port open" \
    || fail "idle: $errs failed cdc-acm reads while idle: $(dmesg_since "$tag" | grep -o ': -1[0-9][0-9]' | sort | uniq -c | tr '\n' ' ')"
  case "$r1" in ANSWERED*) pass "idle: console answers after $IDLE_SECS s idle ($r1)";; *) fail "idle: after $IDLE_SECS s idle: $r1";; esac
  case "$r2" in ANSWERED*) pass "idle: console answers again after 10 s more ($r2)";; *) fail "idle: second probe: $r2";; esac
}

t_timeout() {
  # IDLE_BUSID's CDC data endpoint NAKs while nothing is printed.
  cleanup_vhci; sleep 6
  local vidpid; vidpid=$(bridge_dev "$IDLE_BUSID")
  [ -z "$vidpid" ] && { fail "timeout: $IDLE_BUSID is not attached to the bridge"; return; }
  local lb tty; lb=$(attach "$IDLE_BUSID"); tty=$(tty_of "$lb")
  local ifep; ifep=$(lsusb -v -d "$vidpid" 2>/dev/null | awk '/bInterfaceNumber/{i=$2} /bInterfaceClass/{c=$2} /bEndpointAddress/ && /IN/ && c==10 {print i, $2; exit}')
  local out
  out=$(IFEP="$ifep" VIDPID="$vidpid" LIBUSB_DEBUG=2 python3 - 2>&1 <<'PY'
import ctypes, ctypes.util, os, time
intf, ep = os.environ["IFEP"].split()
intf, ep = int(intf), int(ep, 16)
vid, pid = (int(x, 16) for x in os.environ["VIDPID"].split(":"))
lib = ctypes.CDLL(ctypes.util.find_library("usb-1.0") or "libusb-1.0.so.0")
lib.libusb_open_device_with_vid_pid.restype = ctypes.c_void_p
lib.libusb_open_device_with_vid_pid.argtypes = [ctypes.c_void_p, ctypes.c_uint16, ctypes.c_uint16]
lib.libusb_bulk_transfer.argtypes = [ctypes.c_void_p, ctypes.c_ubyte, ctypes.c_void_p, ctypes.c_int,
                                     ctypes.POINTER(ctypes.c_int), ctypes.c_uint]
for f in ("libusb_detach_kernel_driver", "libusb_claim_interface", "libusb_release_interface",
          "libusb_attach_kernel_driver"):
    getattr(lib, f).argtypes = [ctypes.c_void_p, ctypes.c_int]
lib.libusb_close.argtypes = [ctypes.c_void_p]
lib.libusb_init(None)
h = lib.libusb_open_device_with_vid_pid(None, vid, pid)
lib.libusb_detach_kernel_driver(h, intf); lib.libusb_claim_interface(h, intf)
buf = ctypes.create_string_buffer(64); got = ctypes.c_int(0)
t = time.time(); r = lib.libusb_bulk_transfer(h, ep, buf, 64, ctypes.byref(got), 20000)
print("elapsed %.1f ret %d" % (time.time() - t, r))
lib.libusb_release_interface(h, intf); lib.libusb_attach_kernel_driver(h, intf); lib.libusb_close(h)
PY
)
  local el; el=$(echo "$out" | grep elapsed)
  # WSL's clock can jump, so accept anything well past the old 5 s deadline.
  if echo "$out" | grep -q "urb status -110"; then
    fail "timeout: the bridge timed the idle read out (-110): $el"
  elif echo "$el" | grep -q "ret -7" && echo "$el" | awk '{exit !($2 >= 15)}'; then
    pass "timeout: idle read pending until libusb's own 20 s timeout ($el)"
  else
    fail "timeout: $(echo "$out" | tail -2 | tr '\n' ' ')"
  fi
  # The unlinked read was in flight on the endpoint: the port must still work.
  # libusb only re-attaches the data interface, which cdc-acm does not probe,
  # so re-probe the whole device to get the tty back.
  echo "$lb" > /sys/bus/usb/drivers/usb/unbind 2>/dev/null; sleep 1
  echo "$lb" > /sys/bus/usb/drivers/usb/bind 2>/dev/null
  tty=$(tty_of "$lb")
  local r; r=$(console_probe "$tty" 1)
  case "$r" in ANSWERED*) pass "timeout: console still answers after the unlink ($r)";; *) fail "timeout: console after the unlink: $r";; esac
}

TESTS="${*:-bootsel uf2 esptool msc open idle timeout}"
for t in $TESTS; do echo "##### $t"; "t_$t"; done
cleanup_vhci
[ $FAIL -eq 0 ] && echo "ALL PASSED" || { echo "SOME FAILED"; exit 1; }
