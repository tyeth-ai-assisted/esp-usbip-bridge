# ESP32-P4 USB/IP Bridge (ESP-IDF)

This project is an ESP-IDF scaffold for ESP32-P4 / ESP32-S3 / ESP32-S31 USB dev boards that:

- runs USB Host mode,
- enumerates and exports multiple non-hub USB devices (including devices behind a USB hub),
- exposes it over IP using the Linux USB/IP protocol on TCP port `3240`.

## Status

Implemented now:

- `OP_REQ_DEVLIST` (`usbip list -r <ip>` works when device is attached)
- `OP_REQ_IMPORT` (`usbip attach -r <ip> -b <busid>`)
- `USBIP_CMD_SUBMIT` for control endpoint `EP0` only
- `USBIP_CMD_UNLINK`

Not implemented yet:

- non-control endpoints (bulk/interrupt/isochronous)
- robust recovery for all host and network edge cases

## Protocol Basis

USB/IP wire format is implemented from Linux kernel documentation:

- https://docs.kernel.org/usb/usbip_protocol.html
- https://docs.kernel.org/usb/usbip_protocol.html#architecture

## Project Layout

- `main/main.c`: application startup
- `main/network_init.c`: target-specific network bring-up (Ethernet or Wi-Fi STA)
- `main/usb_backend.c`: USB Host backend, device discovery, EP0 control transfers
- `main/usbip_server.c`: TCP server and USB/IP message handling
- `main/usbip_protocol.h`: protocol constants and packet structs
- `main/Kconfig.projbuild`: board/server configuration options

## Cloning

The `esp-harness` submodule declares its own `esp-idf`, which the build never
reads. Initialise selectively rather
than with `--recursive` to avoid downloading a second full ESP-IDF:

```bash
git clone https://github.com/adafruit/esp-usbip-bridge.git
cd esp-usbip-bridge
git submodule update --init
git -C esp-harness submodule update --init components/scpi_parser/upstream
```

## ESP-IDF Setup

This repository pins ESP-IDF to a commit on upstream `espressif/esp-idf` `master`, because no release tag supports the ESP32-S31 yet (it is still a preview target, so `scripts/build-board.sh` passes `--preview` for it). FS-only USB host mode (`USB_DWC_FSLS_ONLY`) is provided by the `adafruit/esp-usb` fork referenced from `main/idf_component.yml`, so ESP-IDF itself needs no patches.

1. Clone/install ESP-IDF locally for this project:

```bash
./scripts/setup-esp-idf.sh
```

2. In every new shell, load the project-local ESP-IDF environment:

```bash
source ./scripts/idf-env.sh
```

## Build and Flash

Five board profiles are provided:

- `p4-function-ev`: ESP32-P4-Function-EV board, USB/IP over Ethernet
- `m5stack-poe-p4`: M5Stack PoE ESP32-P4, USB/IP over Ethernet
- `p4hil`: [P4HIL](https://github.com/tannewt/p4hil) hardware-in-the-loop
  fixture, USB/IP over Ethernet
- `s3-usb-otg`: ESP32-S3-USB-OTG board, USB/IP over Wi-Fi STA
- `s31-function-coreboard-1`: ESP32-S31-Function-CoreBoard-1, USB/IP over
  gigabit (RGMII) Ethernet, USB host on the USB-HS port

Set your serial port once (example):

```bash
export ESPPORT=/dev/ttyUSB0
```

Console output defaults by target:
- ESP32-P4 boards: USB Serial/JTAG console
- `s3-usb-otg`: `UART0` console (to avoid USB host conflicts)
- `s31-function-coreboard-1`: USB Serial/JTAG console

Once running, a board announces itself over mDNS, so you do not need to know its
address in advance:

```bash
avahi-browse -rt _usbip._tcp
```

### ESP32-P4-Function-EV

Build:

```bash
./scripts/build-board.sh p4-function-ev build
```

Flash and monitor:

```bash
ESPPORT=$ESPPORT ./scripts/build-board.sh p4-function-ev flash
ESPPORT=$ESPPORT ./scripts/build-board.sh p4-function-ev monitor
```

### ESP32-S31-Function-CoreBoard-1

The ESP32-S31 has a single USB OTG controller with a high-speed (UTMI) PHY and
no separate full-speed PHY. Like the P4 boards it is run in FS-only mode, so a
hub on the USB-HS port negotiates at full speed and FS/LS devices behind it work
without split transactions.

Build:

```bash
./scripts/build-board.sh s31-function-coreboard-1 build
```

Flash and monitor:

```bash
ESPPORT=$ESPPORT ./scripts/build-board.sh s31-function-coreboard-1 flash
ESPPORT=$ESPPORT ./scripts/build-board.sh s31-function-coreboard-1 monitor
```

### ESP32-S3-USB-OTG

Before building, edit `sdkconfig.defaults.s3-usb-otg`:

- `CONFIG_USBIP_WIFI_SSID`: your AP SSID
- `CONFIG_USBIP_WIFI_PASSWORD`: your AP password
- `CONFIG_USBIP_WIFI_AUTHMODE`:
  - `2` = WPA2-PSK (recommended for most home/office APs)
  - `3` = WPA2/WPA3 transition mode
  - `0` = open (debug only)
- `CONFIG_USBIP_WIFI_DISABLE_MODEM_SLEEP=y` (recommended for USB host stability)

Build:

```bash
./scripts/build-board.sh s3-usb-otg build
```

Flash and monitor:

```bash
ESPPORT=$ESPPORT ./scripts/build-board.sh s3-usb-otg flash
ESPPORT=$ESPPORT ./scripts/build-board.sh s3-usb-otg monitor
```

Clean board build output and board `sdkconfig` (forces regeneration from defaults on next build):

```bash
./scripts/build-board.sh s3-usb-otg clean
./scripts/build-board.sh p4-function-ev clean
```

If switching targets or recovering from config mismatch, erase flash:

```bash
idf.py -B build-s3-usb-otg -DIDF_TARGET=esp32s3 -DSDKCONFIG=sdkconfig.s3-usb-otg erase-flash
```

## Connect from Linux Host

Install/load USB/IP support on Linux host:

```bash
sudo modprobe vhci-hcd
```

Discover bridge by mDNS:

```bash
avahi-browse -rt _usbip._tcp
```

Resolve host (example):

```bash
avahi-resolve-host-name usbip-xxxxxx.local
```

List and attach exported device:

```bash
usbip list -r <bridge-ip-or-hostname>
sudo usbip attach -r <bridge-ip-or-hostname> -b 1-<devaddr>
```

Example bus ID format from this firmware: `1-1`.

## Service Discovery (mDNS / DNS-SD)

USB/IP protocol itself does not define mDNS discovery. This firmware advertises a custom DNS-SD service:

- Service type: `_usbip._tcp`
- Port: `3240`
- TXT keys: `transport`, `state`, `busid`, `vid`, `pid`, `target`
- `count` indicates the number of exported devices (`busid=multi` when `count > 1`)

Linux discovery examples:

```bash
avahi-browse -rt _usbip._tcp
avahi-resolve-host-name usbip-xxxxxx.local
```

## Notes

- USB/IP in this scaffold is intentionally minimal and aimed at incremental bring-up.
- ESP-IDF version constraint is set in `main/idf_component.yml` to `>=6.0.0-beta2`.
