# ESP32-S31 bridge, controller API and USB host hardening: work log

Hand-off report for resuming this work in a new session. Written 2026-10-07.

## 1. Summary

The work covered three things:
- ESP32-S31-Function-CoreBoard-1 support, merged on top of all the open PRs.
- A controller API for people, scripts and agents: hub port power, device inventory, the I2C strand mux, MCP, SCPI and auth.
- A series of fixes to the USB host stack (the esp-usb fork) found by stress-testing hub port power on a bench with boot-looping DUTs.

Everything below is pushed to forks under `tyeth-ai-assisted`. Nothing has been merged anywhere, and the upstream adafruit repos have not been touched.

**Last thing in progress:** an enumeration timeout. It is configurable, never disables a port permanently, and has diagnostics that tell a device that is *rebooting* from one that is *slow or stalled*. See section 7.

## 2. Repositories, branches and pins

| Repo | Branch | Head | Notes |
|---|---|---|---|
| [tyeth-ai-assisted/esp-usbip-bridge](https://github.com/tyeth-ai-assisted/esp-usbip-bridge/tree/s31-function-coreboard-ethernet) | `s31-function-coreboard-ethernet` | this commit | Fork of adafruit/esp-usbip-bridge. Merges upstream PRs #1 #2 #4 #5 #6 #7 #9. |
| [tyeth-ai-assisted/esp-usb](https://github.com/tyeth-ai-assisted/esp-usb/tree/hub-port-power) | `hub-port-power` | `4e26f44` | Branched from adafruit/esp-usb `fsls-only` (which is ~227 commits behind espressif/esp-usb). |
| [tyeth-ai-assisted/esp-harness](https://github.com/tyeth-ai-assisted/esp-harness/tree/s31-function-coreboard-1) | `s31-function-coreboard-1` | `0e7f989` | Adds the S31 pin table and an `extra_commands` SCPI hook. |
| [Gundry-Consultancy/sbc-mcu-dut-controller](https://github.com/Gundry-Consultancy/sbc-mcu-dut-controller/pull/4) | `feat/esp-usbip-bridge-hosts` | PR #4 | Written by a subagent. Bridge host transport, power, inventory and mux. CI green, 929 tests. |

How the bridge pins its dependencies:
- **esp-idf:** upstream espressif/esp-idf `master` @ `ce2100de`. The S31 is a *preview* target, so the build needs `idf.py --preview`; `scripts/build-board.sh` adds it.
- **esp-harness:** the fork branch above.
- **espressif/usb:** `main/idf_component.yml` points at the esp-usb fork commit `4e26f44`.
- **espressif/cjson:** from the component registry.

## 3. Bench and tooling (Windows host)

### Hardware

- **Board:** ESP32-S31-Function-CoreBoard-1, chip rev v0.0, 16 MB flash.
  - Ethernet: YT8531 PHY over RGMII, link up at 192.168.1.147 (DHCP), hostname `usbip-d07a04`.
  - Consoles (both carry the console):
    - **COM21:** CP2102N USB-UART, UART0. Use RTS to reset.
    - **COM31:** the USB-Serial/JTAG port.
- **USB-HS Type-A port:** two cascaded Genesys GL850G hubs (05e3:0610).
  - Both report **ganged** power switching and PwrOn2PwrGood = 0, so they have no real per-port switches.
  - They show up as `1-1` and `1-1.1`.

DUTs on the hubs (port path = busid):

| Path | Device | Notes |
|---|---|---|
| 1-1.2 | Adafruit QT Py ESP32-S3 (239a:8119) | |
| 1-1.3 | CP2104 (10c4:ea60) | Stable; used for the power-cycle stress tests. |
| 1-1.4 | Raspberry Pi Pico W (2e8a:f00a) | Occasionally reboots on its own; the user worried about it. Data shows it re-enumerates fine (see section 7). |
| 1-1.1.1 | Feather ESP32-S3 Reverse TFT | |
| 1-1.1.2 | Espressif USB JTAG/serial (303a:1001) | **Boot-loops about every 2.7 s.** Useful churn source. |
| 1-1.1.3 | Metro ESP32-S2 | |

### Toolchain and local copies

| What | Where |
|---|---|
| ESP-IDF master | `C:\esp\idf-master`. Tools installed with `install.ps1 esp32s31`. |
| esp-usb fork clone | `C:\esp\esp-usb`, for local edits. |
| Scratch git worktree | `C:\esp\baseline` (pre-controller commit `a669df6`). Can be removed with `git worktree remove C:\esp\baseline`. |

### Building and flashing

- **Build:**
  - Run `tools/bench/idf-s31.ps1 [action]` in PowerShell. From Git Bash, call it as `env -u MSYSTEM powershell.exe -File ...`, because IDF refuses to run under MSYS.
  - The script has hard-coded paths, so edit them for another checkout.
- **Flash:**
  - From `build-s31-function-coreboard-1/`, run `esptool --chip esp32s31 -p COM21 -b 921600 write-flash @flash_args`.
- **Local esp-usb development:**
  - Temporarily set `espressif/usb` in `main/idf_component.yml` to `override_path: "C:/esp/esp-usb/host/usb"`.
  - When done, commit and push esp-usb, then re-pin the git commit.
  - Run `reconfigure build` whenever a Kconfig file changes; otherwise new options don't reach `sdkconfig`.

### Bench scripts (`tools/bench/`)

| Script | What it does |
|---|---|
| `dualmon.py N [uart\|jtag]` | Resets N times while logging both consoles. Reports devices per boot, saves logs of bad boots, and calls `/api/usb/debug` on stalls. |
| `test_enum5s.py N` | Reboot loop with a 5 s enumeration timeout. Reports failures with diagnosis and outcome. |
| `test_abort_retry.py` | Per-port verdicts, plus an attempt to force timeouts with a 1 ms override. |
| `test_timeouts.py` | Settings and override resolution. |

### Gotchas

- **Bash heredocs mangle backslashes** (`\\` and `\n` in Python strings). Write edit scripts to files instead, or use the Edit tool.
- **Orphaned COM port:** a Python test that crashes with a non-daemon serial thread keeps COM21 open. Find it with `Get-CimInstance Win32_Process` (command line `python -`) and stop it.
- **GL850G hubs:** "power off" on these ganged hubs doesn't cut VBUS, but it does disconnect the device logically. It also disturbs other ports on the same hub, which is why `force` is required.

## 4. Work done, by area

### 4.1 S31 board support (bridge `a669df6`)

- **Kconfig:** RMII/RGMII choice and RGMII GPIOs. In `network_init.c`: RGMII EMAC setup plus `USBIP_ETH_PHY_YT8531_INIT` (re-enable autonegotiation; RGMII delays of about 2 ns, as in IDF's `examples/ethernet/basic`).
- **Board defaults:** `sdkconfig.defaults.s31-function-coreboard-1`.
  - PHY reset is GPIO7 and MDC/MDIO are GPIO5/6.
  - The WS2812 LED is on GPIO60.
  - UART0 is the primary console, with USB-Serial/JTAG as secondary.
- **Bug fix:** `esp_efuse_mac_get_default()` writes 8 bytes (an EUI-64) on 802.15.4-capable chips, which overflowed a 6-byte buffer. The fix uses `esp_read_mac(ESP_MAC_BASE)`.
- **Build plumbing:** `build-board.sh` passes `--preview` for this profile, and CI/setup now install esp32s31.

### 4.2 Controller API (bridge `0f6b176` onwards)

Main source files: `main/controller_api.c`, `hub_control.c`, `analog_mux.c` and `scpi_controller.c`.

- **busids** are Linux port paths (`1-1.1.2`) and stay stable across re-enumeration.
  - Topology comes from `usb_device_info_t.parent.dev_hdl`, which the fork now fills in.
  - Hubs come from `usb_host_hub_list()`; hubs are never reported to clients.
- **REST:**
  - Inventory:
    - `/api/usb/devices[/{busid}]` (404 = absent).
    - `/api/usb/hubs` (decoded `wHubCharacteristics`; per port: power, saved state, mismatch, pending action, enumeration timeout).
  - Port power: `/api/ports/{port}/on|off|cycle`, `/api/ports/off|on|restore`. A busy port returns `202 pending` and is applied in the background.
  - Settings: `/api/settings` and `/api/settings/enum_timeout`.
  - Diagnostics: `/api/usb/enum_events` and `/api/usb/debug`.
  - Mux, same API as the CircuitPython mux project: `/api/status`, `/api/select`, `/api/isolate`, `/api/groups/...`, `/api/topology`, `/api/probe`.
  - Other: `/api/auth[/token]`, `/api/reboot`, `/api/schema` and `/ping`. CORS is enabled and real HTTP status codes are returned.
- **MCP:** stateless JSON-RPC at `POST /mcp`, with tools mirroring the REST operations (`list_hubs`, `set_port_power`, `get_enum_events`, `set_enum_timeout`, `mux_*`, ...).
- **SCPI:** on the virtual harness CDC device (busid `2-1`): `HUB:*`, `USB:DEVices?`, `MUX:*`. **Not yet tested on hardware**; this needs a Linux usbip client (fork issue #9).
- **Auth:** an optional Bearer token. Once set, every non-GET request needs it.
- **Web UI:** live device table with power buttons; a Hubs & Power tab with settings toggles, characteristics, saved/mismatch columns and enumeration timeout inputs; an I2C Mux tab; a token field.
- **Settings (all in NVS):**
  - `enforce_per_port_switching` (default on)
  - `restore_port_power` (default on): ports that were switched off stay off across hub reset, power loss and reboot, through the library's port policy, so there is no VBUS glitch.
  - `enum_timeout_ms` (default 2000), plus per-path overrides where the most specific path wins.
- **I2C strand mux:** control bus on GPIO2/3. It needs **external pull-ups**; with only the internal ones, `/api/probe` reports phantom devices.

### 4.3 USB host library (esp-usb fork, `hub-port-power`)

The API is in `include/usb/usb_host_hub.h`:
- `usb_host_hub_list`, `usb_host_hub_get_info` / `_get_port_info` / `_get_snapshot` (batch)
- `usb_host_hub_port_power(addr, port, on, flags)`: FORCE is needed for non-per-port hubs
- `usb_host_hub_set_port_policy` (keep ports off when a hub enumerates)
- `usb_host_set_enum_timeout[_cb]` and `CONFIG_USB_HOST_ENUM_CTRL_TIMEOUT_MS`
- `usb_host_set_enum_event_cb`
- `usb_host_hub_debug_dump`

**Bugs found and fixed.** Except the first, most exist in upstream espressif/esp-usb `master`:

| # | Symptom on the bench | Root cause | Fix |
|---|---|---|---|
| 1 | Hubs invisible to apps; no topology | Hubs are never reported to clients, and `parent_dev_hdl` is always NULL (upstream TODO IDF-10023) | Hub list API; parent handle filled in |
| 2 | `abort()` in `enum_process` (stage IDLE) | `enum_proceed()` processed any reset-completed notification | Only process during SECOND_RESET\*; otherwise mark the node active |
| 3 | Whole USB tree wedged after a quick off/on | Reset/disconnect events went to the first matching device tree node, which was stale | Use the newest node for the port |
| 4 | `assert` in `handle_recycle` (wPortChange != 0) | Port changed again while its device was being freed | Handle the change; defer if a request is in flight |
| 5 | Double control-URB submit, then hub error, then abort | My first fix for #4 handled changes while a feature request was in flight | Recycle defers to the in-flight request |
| 6 | Status bitmap wrong on hubs with >7 ports | `data[i] << i` | Already fixed upstream; the fork is stale |
| 7 | One NAKing device stalled all enumeration (seen about 1 in 10 boots) | No timeout on enumeration control transfers | Timeout. Now **aborts the transfer** (EP0 flush) instead of disabling the port |
| 8 | A device that failed enumeration stayed disabled until replug | ext_port gave up (`dev_reset_attempts = max`) | Retry with back-off (1 s doubling to 30 s) while connected |
| 9 | Power requests took 3 s and blocked everything | Hub-wide busy check plus a 3 s retry loop holding a global lock | Only the target port defers; one round trip, no retries |

What I tried and backed out: processing any port that had pending actions instead of only the head of the list. This broke the rule that only one device may sit at address 0 at a time, and devices failed to enumerate, so it was reverted. Ports must be handled strictly first-in-list.

### 4.4 HTTP 2 s stall (bridge `27fc825`)

This is upstream adafruit/esp-usbip-bridge issue 10, which also affects the P4HIL boards.

- **Cause:** the header read loop only stopped when the buffer *ended* with `\r\n\r\n`. The body usually arrives in the same TCP segment, so the read blocked until `SO_RCVTIMEO` (2 s) expired.
- **Fix:** look for the terminator anywhere with `memmem`.
- **Same commit:** an off-by-one on bodies over 512 bytes, and `strstr` used on a buffer that isn't NUL-terminated.
- **Result:** requests with a body went from 2.000 s to about 20 ms.
- **Not yet reported upstream:** this is waiting on the user's decision.

## 5. Test results (latest builds)

| Test | Result |
|---|---|
| 30 boots, 5 s enumeration timeout (`test_enum5s.py`) | 0 crashes, 0 enumeration failures. 27/30 had all devices; the 3 misses were the Pico mid-reboot. |
| 25 boots, 2 s timeout (before abort/retry) | 25/25 OK. The timeout fired twice on real stalls (CHECK_SHORT_DEV_DESC) and the other devices recovered. |
| Power stress, 40 rounds on 1-1.3 (force) | 40/40 clean, after fixes 2–5 |
| Concurrency: 3 parallel cycles, hammering the boot-looping port, polling | 155 API calls, worst 71 ms; cycles ran in parallel |
| Restore on hub reset | Saved-off port stayed off across a bridge reboot and a downstream hub re-enumeration; disabled-restore showed mismatch and "restore now" fixed it |
| Enumeration timeout settings | Global, per-hub and per-port override resolution correct |
| Per-port verdicts (`/api/usb/enum_events`) | 1-1.1.2 "rebooting" (13 enumerations, 2775 ms interval); Pico 1-1.4 "ok" |

## 6. The user's current concern: the Pico and enumeration timeouts

- **The user asked:** test a 5 s timeout, and make it possible to tell a device that is rebooting and re-enumerating from one that is slow or stalled. Never disable a port permanently; just abort the transaction.
- **How the diagnosis works:**
  - A *rebooting* device disconnects. Its transfer then fails fast (transfer error or disconnect), or it enumerates repeatedly. Either way it shows up as a `rebooting` verdict, with an interval.
  - A *stalled or slow* device stays connected and NAKs. Only the timeout ends that, and it shows as a `timeout` failure.
  - Each failure records what happened next (`re-enumerated` / `failed again`, and after how long).
- **The Pico:** across 30 boots it never failed. It re-enumerates in about 50 ms whenever it reboots, so the bridge isn't failing it.
- **Untested on hardware:** the new abort-and-retry path (fixes 7 and 8 above) compiles but hasn't been exercised. No natural stall happened on this build, and even a 1 ms timeout is longer than any enumeration transfer on this bus. **Next step:** add a stall-injection test hook, for example a debug call that aborts the next enumeration transfer once, and verify abort, retry and recovery, plus the "stalled" verdict.

## 7. Open items, in priority order

1. **Prove abort/retry on hardware** with a stall-injection hook (section 6).
2. **Exclude self-caused enumerations from the "rebooting" verdict.** Enumerations caused by the bridge's own power cycles or restores are currently counted as reboots.
3. **Update sbc-mcu-dut-controller PR #4** for bridge changes made after it was written:
   - treat `202 pending` as success;
   - use `/api/reboot` for host recovery;
   - expose `pending`/`mismatch`/settings and the enumeration timeout;
   - the agent's bench-verification items (re-attach timing on a short ESP32-S3 boot window, stable tty names, 409 on GL850G).
4. **Intermittent HTTP connection reset** (fork issue #10). Seen once since the 2 s fix; cause unknown, possibly lwIP RST on close.
5. **Remaining library follow-ups:** debug dump log level (esp-usb #7); retry/timeout behaviour for root-port devices (only external-hub ports retry today).
6. **Before upstreaming:** rebase esp-usb `hub-port-power` onto espressif/esp-usb `master` (the FS-only patch is superseded by upstream's `fsls_only`); repoint fork URLs; pin ESP-IDF to a release once one supports the S31 (bridge issue #8).
7. **Inherited issues still open:** legacy endpoints don't escape JSON (#4); the GPIO tab only lists P4HIL-style pins (#5); stale README status (#6); CI gaps (#7); esp-harness's own ESP-IDF submodule (esp-harness #1).

## 8. Decisions waiting on the user

- **Upstream issue 10:** post the root cause and fix on adafruit/esp-usbip-bridge, as a comment or a PR? Not done. Fork issue #12 deliberately doesn't link to it, to avoid a public cross-reference.
- **PR #4:** whether to update it for the items in section 7.3.

## 9. Issues on the forks

Every issue carries a provenance label: `upstream-inherited`, `from-our-changes` or `fixed-on-branch`.

- **esp-usbip-bridge:** #1–#12. #12 is the HTTP stall.
- **esp-usb:** #1–#8. #8 is the enumeration timeout; #7 is the API follow-ups (busy hub done; log level open).
- **esp-harness:** #1–#2.
