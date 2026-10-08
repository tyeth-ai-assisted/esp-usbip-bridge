#!/bin/bash
# USB/IP client regression for the S31 bench (usb-storage left bound):
#   bootsel  RP2040 (RP2040_BUSID, CircuitPython) -> 1200-baud touch -> BOOTSEL:
#            no usb-storage reset loop, RPI-RP2 detected, picotool info, picotool reboot
#   uf2      picotool save -a of the whole flash, then copy it back as a UF2 (4 MiB write)
#   esptool  esptool --no-stub flash_id twice on one attach (ESPTOOL_BUSID), no "cannot find a urb"
#   msc      read/write 64 KiB on each mass-storage drive in MSC_BUSIDS
#   timeout  a 20 s libusb bulk read on an idle CDC data endpoint ends after the bridge's 5 s
#            deadline with URB status -110 (ETIMEDOUT)
#
#   regression.sh [test ...]     (default: all, in the order above)
. "$(dirname "$0")/lib.sh"
RP2040_BUSID="${RP2040_BUSID:-1-1.4}"
ESPTOOL_BUSID="${ESPTOOL_BUSID:-1-1.3}"
MSC_BUSIDS="${MSC_BUSIDS:-1-1.1.3 1-1.2}"
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

t_timeout() {
  # The RP2040's CircuitPython CDC data endpoint NAKs while nothing is printed.
  cleanup_vhci; sleep 6
  if [ "$(bridge_dev "$RP2040_BUSID")" = "2e8a:0003" ]; then
    # Left in BOOTSEL by an earlier test: back to the application
    attach "$RP2040_BUSID" >/dev/null; sleep 5
    timeout 30 picotool reboot >/dev/null 2>&1; sleep 2; cleanup_vhci
  fi
  wait_bridge_dev "$RP2040_BUSID" 2e8a:f00a 20 || { fail "timeout: $RP2040_BUSID is not running its application"; return; }
  attach "$RP2040_BUSID" >/dev/null; sleep 3
  local ifep; ifep=$(lsusb -v -d 2e8a:f00a 2>/dev/null | awk '/bInterfaceNumber/{i=$2} /bInterfaceClass/{c=$2} /bEndpointAddress/ && /IN/ && c==10 {print i, $2; exit}')
  local out
  out=$(IFEP="$ifep" LIBUSB_DEBUG=2 python3 - 2>&1 <<'PY'
import ctypes, ctypes.util, os, time
intf, ep = os.environ["IFEP"].split()
intf, ep = int(intf), int(ep, 16)
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
h = lib.libusb_open_device_with_vid_pid(None, 0x2e8a, 0xf00a)
lib.libusb_detach_kernel_driver(h, intf); lib.libusb_claim_interface(h, intf)
buf = ctypes.create_string_buffer(64); got = ctypes.c_int(0)
t = time.time(); r = lib.libusb_bulk_transfer(h, ep, buf, 64, ctypes.byref(got), 20000)
print("elapsed %.1f ret %d" % (time.time() - t, r))
lib.libusb_release_interface(h, intf); lib.libusb_attach_kernel_driver(h, intf); lib.libusb_close(h)
PY
)
  echo "$out" | grep -q "urb status -110" && pass "timeout: bridge deadline reaches Linux as -110 ($(echo "$out" | grep elapsed))" \
    || fail "timeout: $(echo "$out" | tail -2 | tr '\n' ' ')"
}

TESTS="${*:-bootsel uf2 esptool msc timeout}"
for t in $TESTS; do echo "##### $t"; "t_$t"; done
cleanup_vhci
[ $FAIL -eq 0 ] && echo "ALL PASSED" || { echo "SOME FAILED"; exit 1; }
