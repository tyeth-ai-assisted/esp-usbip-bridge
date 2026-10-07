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

## Controller API (HTTP, MCP, SCPI)

Besides USB/IP, the bridge serves a controller API on port 80 for people
(the web page), scripts and agents. `GET /api/schema` lists every endpoint.

**Bus IDs are Linux style port paths** (`1-1.3` = port 3 of the hub on the
root port), so a device keeps its busid when it re-enumerates, and
`usbip list -r` / `usbip attach -b` work like against a Linux host.
Virtual devices are on bus 2.

### USB devices and hub port power

| Method | Path | |
|---|---|---|
| GET | `/api/usb/devices` | VID/PID, manufacturer/product/serial strings, speed, MaxPower, interfaces, hub/port and port power state |
| GET | `/api/usb/devices/{busid}` | one device, 404 when absent (presence check) |
| GET | `/api/usb/hubs` | hubs with their decoded `wHubCharacteristics` (power switching `per-port`/`ganged`/`none`, over-current mode, compound, TT think time, port indicators, PwrOn2PwrGood, controller current) and every port's state, saved state and mismatch |
| GET | `/api/ports`, `/api/ports/{port}` | flat port list (also at `/ports`) |
| POST | `/api/ports/{port}/on`, `/off` | SetPortFeature/ClearPortFeature PORT_POWER, body `{"force":bool}`. `202` with `"pending": true` when the port is busy (e.g. its device is enumerating): the request is applied in the background |
| POST | `/api/ports/{port}/cycle` | off, wait `off_ms` (default 1000), on; runs in the background |
| POST | `/api/ports/off`, `/api/ports/on` | every per-port switched port that does not lead to a hub |
| POST | `/api/ports/restore` | switch every port whose power differs from its saved state back to it |
| GET/POST | `/api/settings` | `{"enforce_per_port_switching": bool, "restore_port_power": bool, "enum_timeout_ms": int}` |
| POST | `/api/settings/enum_timeout` | `{"path": "1-1.3", "timeout_ms": int\|null}`: enumeration timeout override for a hub or port path and everything behind it (no path = global, `null` removes) |
| POST | `/api/usb/debug` | log the USB host's hub, port and enumeration state to the console |

Port power is refused (409) unless `force` is set when the hub does not report
per-port power switching (ganged hubs switch all ports or none, and many cheap
hubs have no switches at all) while `enforce_per_port_switching` is on (the
default), and when powering off a port that leads to another hub.

The last power state set for each port path is saved in NVS. With
`restore_port_power` on (the default), a port that was switched off stays off
when its hub re-enumerates: hub reset, upstream power loss or bridge reboot.
The USB host library keeps such ports unpowered from the start, so the DUT never
sees VBUS. With it off, ports come back powered and show `mismatch` until
`POST /api/ports/restore`. Both settings are toggles on the web page's
Hubs & Power tab.

Requests never wait for a busy port: requests for a port whose device is
enumerating are queued and applied in the background (each port on its own),
and power cycles of several ports run in parallel.

**Enumeration timeout.** Devices are enumerated one at a time, so a device that
never answers would stop every other device from enumerating. If an enumeration
control transfer does not complete within `enum_timeout_ms` (default 2000 ms,
0 = no timeout) the device's port is disabled and enumeration carries on; power
cycle the port to try that device again. USB 2.0 gives a device 500 ms for the
first data packet of a request and 50 ms for a status stage, so 1000 ms is the
shortest sensible global value; slow-booting boards that NAK while starting up
can get a longer per-port or per-hub override (Hubs & Power tab, or
`POST /api/settings/enum_timeout`).

### I2C strand mux

A C port of
[sbc-dut-analog-mux-api-circuitpy](https://github.com/Gundry-Consultancy/sbc-dut-analog-mux-api-circuitpy)
with the same API, so its existing clients (including the HIL controller's
`select_i2c_strand` stage) can point at the bridge: `GET /api/status`,
`GET /api/duts`, `POST /api/select {"dut"}`, `POST /api/isolate`,
`POST /api/groups/{group}/select/{channel}`, `POST /api/groups/{group}/channel`,
`GET|PUT /api/topology`, `GET /api/probe`. ADG729 (dual 4:1) and ADG728 pairs
(8:1 SDA + 8:1 SCL) are supported; selection is break-before-make across all
groups. The topology is stored in NVS. The control bus pins are
`CONFIG_USBIP_MUX_I2C_SDA_GPIO`/`SCL_GPIO` (GPIO2/GPIO3 on the
ESP32-S31-Function-CoreBoard-1, unset elsewhere). The bus needs external pull-ups
(about 4.7 kOhm); with only the internal ones `/api/probe` reports phantom
devices.

### Auth

`POST /api/auth/token {"token": "..."}` sets a token (empty clears it). Once set,
every non-GET request (and `/api/select/*`) needs `Authorization: Bearer <token>`.
The web page has a token field.

### MCP

`POST /mcp` is a stateless Model Context Protocol endpoint (Streamable HTTP,
JSON responses) with tools `list_usb_devices`, `get_usb_device`, `list_hubs`,
`set_port_power`, `power_cycle_port`, `set_device_name`, `mux_status`,
`mux_select`, `mux_isolate`, `mux_set_channel`, `mux_get_topology`,
`mux_set_topology`, `get_port_power_settings`, `set_port_power_settings`,
`set_enum_timeout`, `restore_port_power` and `get_bridge_info`. For example with Claude Code:

```bash
claude mcp add --transport http usbip-bridge http://usbip-xxxxxx.local/mcp
```

Add `--header "Authorization: Bearer <token>"` when a token is set.

### SCPI

The virtual test harness device (USB/IP busid `2-1`, CDC-ACM) also accepts
`HUB:LIST?`, `HUB:PORT:POWer "1-1.3",ON|OFF[,FORCE]`, `HUB:PORT:POWer? "1-1.3"`,
`HUB:PORT:CYCLe "1-1.3"[,<off_ms>[,FORCE]]`, `USB:DEVices?`, `MUX:SELect "dut"`,
`MUX:SELect:CHANnel "group",<n>`, `MUX:ISOLate`, `MUX:ACTive?` and
`MUX:STATus?` (JSON answers for list queries).

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
