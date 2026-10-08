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

## 10. 2026-10-07 evening: USB/IP client fixes (WSL2 `vhci_hcd`, mass storage)

Tested from a WSL2 Ubuntu 24.04 (kernel 6.6) usbip client with `usb-storage` left bound. Final image `ef312b8`; esp-usb pinned to `afe9c46`.

| Issue | Commit | What |
|---|---|---|
| esp-usb #9 | `f99f008` | `usb_host_endpoint_reset_toggle()`: DATA0 on a halted non-control pipe (`usb_dwc_hal_chan_set_pid`) |
| esp-usb #10 | `f03bba3` | A port that reconnects before it is recycled is reset and enumerated instead of left idle |
| esp-usb #11 | `afe9c46` | No second hub request while one is in flight (enum retry ran outside the hub cycle, then double URB submit and `abort()`) |
| bridge #14 | `18c2355` | `to_linux_errno()`: ETIMEDOUT 110, EMSGSIZE 90, ESHUTDOWN 108, ... |
| bridge #13 | `8035926` | CLEAR_FEATURE(HALT) / SET_INTERFACE / SET_CONFIGURATION applied to host pipes (toggle reset); halted endpoint gives `-EPIPE` |
| bridge #17 | `345c78a` | Aborted control transfers are no longer freed while in flight (heap corruption and reboots) |
| bridge #18 | `84ddf83` | Bulk URBs up to 128 KiB (`CONFIG_USBIP_MAX_URB_SIZE`) held in 4 KiB segments; RET_SUBMIT has no payload copy |
| bridge #20 | `ef312b8` | A device's interfaces are released when its USB/IP session ends (16 DWC channels) |

Results:
- RP2040 BOOTSEL: RPI-RP2 mounts; `picotool info/save/reboot` work; a 4 MiB UF2 copy flashes the board.
- 15x 1200-baud BOOTSEL loop: no stuck port, no reboot.
- TinyUSB mass storage (Metro S2, QT Py S3) mounts read/write.
- `esptool` x2 on one attach works.
- A forced timeout reaches Linux as -110.

Still open:
- bridge #19: idle bulk IN times out at 5 s, and sibling URBs are reported as `-ECONNRESET`, so cdc-acm stops reading. Needs a decision.
- bridge #20: the channel budget with several devices attached at once.
- `ext_hub` `abort()` on a failed hub.
- sbc-mcu-dut-controller#6: the `1-1.1.2` DUT, probably an ESP32-S3 whose app takes over the USB PHY and crashes. Not reflashed.

Bench notes:
- The Tachyon renegotiates USB-PD (5 V/9 V) when its 5G link dips. Its whole USB hub drops (CP2102N, S31 USB-JTAG, USB LAN), so SSH over Tailscale (100.101.245.66) is the most reliable route.
- Flash at 460800, refuse to flash while anything holds the port, and check for "Hash of data verified". A wedged CP2102N (`can't set config #1, error -32`) needs a physical replug.
- Some CircuitPython/WipperSnapper drives have no `CIRCUITPY` label (`WIPPER`, or none). Find them by transport and filesystem type, not by label.

## 11. 2026-10-08: PSRAM, EP0 cancel, bench tools

- **PSRAM** (`15ec74c`):
  - The board's ESP32-S31-WROOM-3 (ESP32-S31NRV16) has 16 MB of in-package 1.8 V octal PSRAM (AP Memory gen 4). The datasheet v0.7 (Table 6-11) rates it at ≥ 200 MHz, so it runs at 200 MHz and passes the boot memory test.
  - With PSRAM on, the MPLL runs at 400 MHz, which can't give RGMII its 125 MHz, so the RGMII TX clock now comes from the APLL. Free heap went from ~270 KiB to ~17 MiB.
  - The eFuses (BLOCK1) are blank on this engineering sample (MAC 00:..), so they can't report PSRAM.
- **Full config committed** as `sdkconfig.s31-function-coreboard-1.example`. `build-board.sh` and `idf-s31.ps1` refresh it after each build.
- **EP0 cancel** (`b29ed79`, esp-usb `fc036c8`, bridge #21): a control transfer the device never finishes is cancelled instead of orphaned, so it no longer blocks the device's EP0 or the pipe pool.
- **Bench tools** in `tools/bench/`:
  - `tachyon-flash.sh` and `tachyon-console.sh`;
  - `usbip-client/regression.sh` (BOOTSEL/picotool/UF2, esptool, mass storage, -110 timeout);
  - `usbip-client/esp-usbjtag-catch.sh`.
  Session scratch directories got wiped, so keep scripts here.
- **Open:**
  - bridge #19: idle IN reads hold slots, and the 8-in-flight cap lets them starve OUT and control. This is what stops esptool syncing with the `1-1.1.2` DUT in its ROM loader. Needs a decision.
  - sbc-mcu-dut-controller#6: DUT not identified yet.
  - The QT Py at `1-1.2` (WipperSnapper) reboots itself every few minutes. That's normal for it, and it occasionally makes the mass-storage test flaky.

## 12. 2026-10-08 evening: pending reads without deadlines, task-free URB path (bridge #19)

### The problem

Linux keeps bulk/interrupt IN URBs pending for ever (cdc-acm: 16 reads per port). The bridge gave every transfer a 5 s deadline, held each pending read in a worker task and a pool slot, let a connection have only 8 URBs in flight, and aborted a timed-out read by halting and flushing its endpoint, which cancelled the sibling reads as `-ECONNRESET` (cdc-acm then stops resubmitting them). Measured on the old image (`2f12b2d`): a pyserial open of an idle cdc-acm port took 9 to 18 s, an idle console stopped answering after 20 s, and esptool could not catch the `1-1.1.2` DUT's ROM loader in 60 s.

### Options compared

| | 1. raise limits (48 URBs, 64 slots, PSRAM stacks) | 2. reserve slots for OUT/control | 3. no IN deadline + task-free backend (shipped) |
|---|---|---|---|
| serial open | fast while reads complete | fast | fast |
| idle reads | still time out at 5 s; siblings still `-ECONNRESET` | same | never time out; one read is cancelled on its own |
| memory per pending read | 8 KiB task stack (PSRAM on the S31, internal RAM elsewhere) | 8 KiB task stack | about 0.5 KiB (request + buffer) |
| boards without PSRAM | 16 pending reads = 128 KiB | same | fine |
| esptool on an idle ROM loader | works while the pool is not full | works | works |

Options 1 and 2 both keep the two defects behind #19 (the deadline and the sibling cancel) and both make a pending read cost a task. Option 3 removes the task, so the limits can be generous everywhere, and matches Linux semantics. Measurements are in #19.

### What changed

- **esp-usb `a391e1f`** (`hub-port-power`): `usb_host_transfer_cancel()` retires one queued bulk/interrupt transfer (`hcd_urb_cancel_pending()` on the endpoint's pipe, then the endpoint callback). A transfer already in one of the pipe's two DMA buffers reports `ESP_ERR_NOT_FINISHED`; only those need a halt/flush.
- **Backend** (`main/usb_backend.c`): the blocking `usb_backend_*_transfer()` calls are gone. `usb_backend_submit(req)` queues a `usb_backend_req_t`; the backend task admits requests into an in-flight list in order, submits them, and calls `req->done()` from its own task. `usb_backend_cancel(req)` retires a waiting or in-flight request. Rules:
  - bulk/interrupt transfers have **no deadline**; control transfers keep one (`CONFIG_USBIP_CTRL_XFER_TIMEOUT_MS`, 5000, 0 = none);
  - at most `CONFIG_USBIP_MAX_IN_XFERS_PER_ENDPOINT` (8) reads in flight per IN endpoint, the rest wait in the queue; reads as a whole leave 8 of `CONFIG_USBIP_MAX_INFLIGHT_XFERS` (128 on P4/S31, 64 elsewhere) free for OUT and control, so a read can never delay them;
  - a cancelled read that is only queued is retired alone; one in flight gets its endpoint halted and flushed, and the sibling transfers that completed as `CANCELED` without being cancelled are **submitted again in order** (up to 8 times) instead of being reported `-ECONNRESET`;
  - orphaning (#17) stays as the last resort after a 1 s cancel wait; when an orphan finally retires after its session ended, the device's interfaces are released then (`release_pending`), closing a gap in #20;
  - the task blocks in `usb_host_client_handle_events()` and is woken by `usb_host_client_unblock()` on submit/cancel, so completions are processed at once instead of on the 10 ms poll.
- **Server** (`main/usbip_server.c`): one reader (the connection task) and one **writer task** per connection; no task per URB. The reader validates, reads OUT data and submits; the backend's completion queues the URB for the writer, which sends `RET_SUBMIT`/`RET_UNLINK` and frees it. Large bulk URBs chain their segments from the completion callback. `CONFIG_USBIP_MAX_INFLIGHT_URBS` (64) bounds memory per connection; `CMD_UNLINK` now also cancels the backend request.
- **Kconfig:** `USBIP_NUM_PIPES` removed (its "match the channel count" rationale was wrong: transfers queue on their endpoint's pipe, channels are per endpoint); the four options above added. `sdkconfig.s31-function-coreboard-1.example` refreshed.
- **Bench:** `regression.sh` gained `open` (serial open under 1 s on `1-1.4` and `1-1.1.1`), `idle` (25 s + 10 s idle on `1-1.1.1`, cdc-acm dynamic debug counts failed reads, console still answers) and the `timeout` test now expects libusb's own 20 s timeout (`-7`) with no `-110`, then a working console. `lib.sh`: `tty_of` waits for the driver, `console_probe`, `serial_open_times`, `acm_debug_on`. `idf-s31.ps1` finds its own checkout.

### Review of the earlier timeout and signalling changes

Asked for by the user. Each item says what it did, whether it survives, and why.

| Change | Kept? | Notes |
|---|---|---|
| 5 s software deadline on every transfer (inherited) | control only | Linux has no HCD deadline; `usb_control_msg()` callers unlink at 5 s themselves, so the bridge's control deadline is a safety net for clients that never unlink. Bulk/interrupt deadlines were the cause of #19. |
| Abort by halt + flush of the endpoint, 50 x 10 ms inline pump (inherited) | replaced | Now a per-URB cancel first; a flush only for an in-flight read, one per endpoint per pass, no inline pumping (the callbacks arrive on the next `handle_events`). Siblings are resubmitted, not failed. |
| #14 Linux errno mapping | kept | Unchanged. |
| #15 `CMD_UNLINK` answered with `RET_UNLINK(-ECONNRESET)` instead of `RET_SUBMIT`; `RET_UNLINK(0)` when already answered | kept | The writer frees the slot under `write_mutex`, same invariant. The unlink now reaches the backend as a cancel at once instead of being polled from a flag. |
| #13 STALL leaves the endpoint halted (`-EPIPE`); `CLEAR_FEATURE`/`SET_INTERFACE`/`SET_CONFIGURATION` reset host toggles | kept | A sibling flushed by a STALL is resubmitted and refused with `-EPIPE` by the halted check, same result as before. |
| #17 orphaning of aborted transfers still in flight | kept as fallback | Still needed: a bulk OUT the device NAKs for ever cannot be retired until the halt completes. The orphan now also triggers the deferred interface release. |
| #18 segmented bulk URBs | kept | Segments chain from the completion callback; a cancel between segments ends the URB with `-ECONNRESET` as before. |
| #20 interface release at session end | kept and fixed | Release was skipped when an orphan was still in flight and never retried; now deferred to the orphan's retirement, and dropped if a new session starts. |
| #21 EP0 cancel, `-EAGAIN` retry while EP0 recovers, collateral resubmit (3x) | kept | Shares the generic resubmit path (limit 8). The retry ends at the control deadline. |
| Pool of 16 "pipe" slots guarded by a counting semaphore | replaced | Workers blocked on the semaphore; now an ordered queue in the backend, admission by share. |
| 8 URBs in flight per connection, 50 ms polling for a slot | replaced | 64 (Kconfig), a counting semaphore the writer gives. |
| Worker task per URB (8 KiB) | removed | Reader + writer per connection. |
| Backend event queue 16, 10 ms poll | kept | Completions and submits wake the task; the poll only serves deadlines and cancel waits. |
| 5 s wait for workers at session end | kept | Now waits for the backend to answer the cancelled URBs; orphans are answered within the 1 s cancel wait. |

Still worth a look (filed as an issue): no TCP keepalive on the URB stream, so a client that vanishes without a FIN keeps the device and its interfaces claimed until the socket errors.

Baseline on the old image `2f12b2d` with the new tests (`regression.sh open idle timeout`): opens 16.9 s (`1-1.4`) and 15.8 / 10.5 / 10.4 s (`1-1.1.1`); 29 cdc-acm reads ended with `-104` ("urb shutting down") during a 35 s idle on `1-1.1.1` (the console still answered, as the timed-out read itself is resubmitted, so one read survives); the libusb read got `-110` after 5.2 s and cdc-acm's re-probe took over 15 s.

### Results

Measured on the S31 bench from the WSL2 client (`regression.sh open idle timeout`, the esptool catch script, a heap probe through MCP `get_bridge_info`):

| | old image `2f12b2d` | option 1 (old code, 48 URBs, 64 slots, PSRAM stacks) | option 2 (old code, reader never blocks on IN, IN limited to 12 of 16 slots) | shipped |
|---|---|---|---|---|
| pyserial open, idle cdc-acm port | 9.3 to 17.9 s | 15 to 35 ms | 10 to 32 ms | 9 to 16 ms |
| 35 s idle: cdc-acm reads given up | 29 (-104) | 30 (-104) | console silent at 25 s, answered after 3.5 s at 35 s (the -110 timeouts were not counted by the test then) | 0 |
| libusb 20 s read on the idle endpoint | -110 at 5.2 s | -110 at 5.2 s | bridge -110 after 3.9 s; console silent after the unlink | -7 at 20.0 s, console works after the unlink |
| esptool catch of `1-1.1.2` (60 s) | not caught, 22 re-attaches | caught in 6 s | caught after 27 s (10 re-attaches, 5 esptool rounds) | caught in 3 s, first attach |
| heap per pending read | 8 KiB stack (internal) | 8 KiB stack (PSRAM), and the quick patch leaked (needs `vTaskDeleteWithCaps`) | 8 KiB task stack (internal RAM): 17 pending reads = 148 KiB, returned on close | about 1 KiB; 17 reads = 17 KiB, all back on detach |

Final image `1f87edb` (clean build, esp-usb pinned to `a391e1f`): `regression.sh` **all 20 checks pass** (BOOTSEL + picotool + 4 MiB UF2 copy in 98 s; esptool x2; mass storage on `1-1.1.3` and `1-1.2`; open 9 to 17 ms; idle 35 s with 0 failed reads; libusb read pending to its own 20 s timeout, console alive after the unlink). No hub re-enumeration during the run; 17 pending reads cost 17 KiB and the heap returns to 17.05 MB on detach.

The `1-1.1.2` DUT (sbc-mcu-dut-controller#6) is an ESP32-S3 (QFN56, v0.1) with 4 MB embedded XMC flash and 2 MB embedded PSRAM, MAC f4:12:fa:59:5d:b0, and its flash is **erased** (bootloader and partition table all 0xFF): the ROM has nothing to boot and its watchdog resets it every 2.7 s. Not reflashed; waiting on the user.

Bench notes: the Tachyon's USB-C dock put the Tachyon into device/sink mode after a PD renegotiation (host controller deregistered, `data_role: host [device]`); the UCSI data-role swap failed and a reboot of the Tachyon (authorised by the user) restored host mode. Every CDC DUT runs WipperSnapper, so there is no REPL: the quiet Feather at `1-1.1.1` answers Ctrl-C/Enter with its fatal-error line and serves as the idle console. During the first full regression both hubs re-enumerated twice within 7 s right after the QT Py mass-storage test (every device got a new address); it did not recur in the later runs or on the old-code images, so it looks like a power event on the bus-powered GL850G chain when the QT Py rebooted after its filesystem changed, not a bridge regression. Worth watching.
