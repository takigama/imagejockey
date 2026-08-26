# Under the hood

## Hardware

| Function | GPIOs |
|---|---|
| TFT (ST7735, SPI2) | MOSI=3, SCLK=5, CS=4, DC=2, RST=1, Backlight=38 (active-LOW) |
| microSD (SDMMC, 4-bit) | CLK=12, CMD=16, D0=14, D1=17, D2=21, D3=18 |
| Button | GPIO0 (the board's only button — shared with the BOOT/download-mode strap pin) |

16MB flash, no PSRAM (confirmed empirically — the firmware doesn't depend on it, but see "Memory
constraints" below for why that matters more than usual on this board).

## Project layout

- `main/` — firmware source.
  - `msc_disk.c` — raw TinyUSB `tud_msc_*` callbacks, backed by whichever file `media.c` currently has
    mounted (or raw SD sectors, in passthrough mode).
  - `media.c` — the shared, mutex-guarded "which image is mounted, read-only or writable, passthrough
    or not" state, used by both the USB side and the WiFi/UI side. Also owns display-name storage
    (`.imgnames.tsv` on the SD card) and blank-image creation.
  - `display.c` / `sdcard.c` / `button.c` — board drivers.
  - `ui.c` — the on-device carousel (3-row, centered-cursor) and blank-image-size picker.
  - `wifi.c` — SoftAP+STA dual mode, credential storage (NVS, deliberately unencrypted — see the
    project's own judgment call on this, it's a personal-device threat model).
  - `web.c` — the management web page: image list/mount/rename/delete/create, WiFi credentials, OTA
    check/update, `/log` (RAM ring-buffer log capture, since this board has no way to reach a serial
    console during normal MSC-serving operation — see `logbuf.c`), `/heap` (memory snapshot).
  - `ota.c` — GitHub-Releases-based update check and download; see "OTA & memory constraints" below for
    why this is more involved than a typical `esp_https_ota()` call.
  - `app_main.c` — boot sequence, including the pending-update reboot cycle (see below).
- `components/esp_tinyusb_core/` — a trimmed **local fork** of `espressif/esp_tinyusb`, with its
  storage-wrapper files removed. That component's own `tud_msc_*` implementations are compiled in
  unconditionally whenever its `CONFIG_TINYUSB_MSC_ENABLED` Kconfig flag is on — which is also the *only*
  way to get the underlying TinyUSB MSC class code — so there's no way to use its convenient
  `tinyusb_driver_install()` alongside our own file-backed-disk callbacks without this fork. See the
  comment at the top of that component's `CMakeLists.txt`.
- `certs/github_ota_roots.pem` — a trimmed TLS root-CA bundle (just the two roots OTA actually needs:
  USERTrust ECC for `github.com`, ISRG Root X1/Let's Encrypt for the `release-assets.githubusercontent.com`
  redirect target) instead of mbedTLS's full ~100-cert default bundle. Not currently wired up to
  anything — see "OTA & memory constraints" for why cert validation is off entirely right now.
- `docker/`, `docker-compose.yml`, `scripts/` — the WSL+Docker build/flash toolchain (see `build.md`).
- `partitions.csv` — OTA-capable layout: `nvs` (24K), `otadata` (8K), `phy_init` (4K), then two 2MB app
  slots (`ota_0`/`ota_1`) instead of the default single-factory-app table.

## Memory constraints & OTA

This board has no PSRAM — everything (WiFi driver buffers, the TFT, SD card I/O, the web server, TinyUSB
MSC's own descriptors/buffers, and TLS for OTA) shares roughly 300KB of internal SRAM. That's normally
plenty, but **installing the TinyUSB MSC drive and attempting a firmware download at the same time isn't**
— mbedTLS's handshake/record buffers plus TinyUSB's static allocations together were reliably exhausting
it, surfacing as a confusing `esp_http_client` "Out of buffer" error deep inside the *second* TLS
connection (following GitHub's redirect to the actual asset host) that had nothing to do with the actual
cause. A few things were tried and are still in place because they genuinely help, even though none of
them alone was enough with the drive active:

- `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y` — mbedTLS allocates its ~16KB TLS record buffer only while actively
  needed and frees it between reads/writes, instead of holding it for the whole connection.
- The trimmed cert bundle in `certs/github_ota_roots.pem` (currently unused — see below).
- Cert validation is **disabled entirely** for OTA right now (`CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP=y` and
  `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y`, plus no `crt_bundle_attach`/`cert_pem` set on either HTTP
  client in `ota.c`). TLS is still encrypted, just not authenticated — acceptable for now given the
  actual fix below, but worth re-enabling (wire the trimmed bundle back in) once there's more headroom
  to spare.

The fix that actually closes the gap is architectural, not a buffer tweak: **OTA updates run with the
USB drive not installed.** `main/ota.h`/`ota.c` split the update flow in two:

- `ota_check_for_update()` — a small GitHub API JSON request (just the latest release's `tag_name`,
  compared against `FIRMWARE_VERSION`, a build-time `git describe` string — see top-level
  `CMakeLists.txt` and `build.md`'s note on tag ordering). Cheap enough to run with the drive active;
  `web.c` exposes this as `/ota/check`.
- `ota_update_from_github()` — the actual download+flash. `web.c`'s `/ota/update` doesn't call this
  directly; it calls `ota_schedule_update_reboot()`, which stashes a flag in `RTC_NOINIT_ATTR` memory
  (survives the upcoming soft reset, but — deliberately — resets to 0 on power loss, so a crash mid-update
  can't get stuck retrying forever) and reboots. `app_main.c` checks that flag right after `logbuf_init()`,
  and if set: skips installing TinyUSB MSC for that one boot (same code path as the BOOT-held debug mode,
  see `build.md`), waits for WiFi to associate, runs the real update, and reboots again either way —
  back to a normal, MSC-enabled boot. On success `ota_update_from_github()` reboots into the new image
  itself; on failure `app_main.c` reboots back to normal anyway rather than sitting in no-MSC mode.

Net effect from the web UI: "Check for update" is instant and doesn't interrupt whatever's using the
drive; "Update now" causes a visible (from the host's perspective) drive disconnect for the duration of
the download, then it's back.

### A note on debugging this board

Two things that cost real time working out and are worth knowing going in:

- **`/log` (RAM ring-buffer log capture) can look like it's "losing" specific log lines** if the
  firmware you're actually running doesn't match the source you're looking at — e.g. a build that
  silently landed on the wrong OTA partition (see `build.md`'s note on `esptool`'s multi-region
  `write-flash` occasionally dropping a region), or `FIRMWARE_VERSION` quietly falling back to a stale
  value because `git describe` failed inside the Docker container (this actually happened — see the
  "dubious ownership" fix in `docker/Dockerfile` — and produced a version string that looked plausible
  but wasn't reflecting the checked-out commit). If a log line you just added isn't showing up, first
  confirm — via a fresh `strings build/imagejockey.bin | grep ...` and, if needed, a flash-content
  read-back with `esptool read_flash` — that the binary actually on the chip is the one you think it is,
  before suspecting the logging path itself.
- **The BOOT button can't reliably force the no-MSC debug/console mode** despite `app_main.c` supporting
  it (`boot_button_held()`) — GPIO0 is also the ROM bootloader's own strapping pin, sampled *before*
  `app_main()` ever runs, so holding it at power-up almost always wins download mode instead. There's no
  way to time a manual press to land after that check but still within the function's early ~50ms
  window. If you need the console for a debugging session, temporarily hardcode
  `debug_mode = true` in `app_main()` instead (and revert it afterward — it disables the drive on every
  boot, which defeats the point of the firmware if left in).

## Status / roadmap

- [x] Phase 0 — project scaffold + Docker toolchain
- [x] Phase 1 — display + SD + button bring-up, standalone image browsing
- [x] Phase 2 — USB MSC boot mode, including writable `.img` support and on-device blank-image creation
- [ ] ~~Phase 3 — USB drag-and-drop file-transfer mode~~ — superseded by WiFi upload, not built
- [x] Phase 4 — WiFi provisioning + web upload/selection + OTA
- [ ] Phase 5 — polish (on-screen WiFi/mode status, low-SD-space handling, more error states)
- [ ] exFAT support (removes the 4GiB−1 per-file limit; needs a local `fatfs` component fork — see
  `ff.c`'s `FF_FS_EXFAT`, hardcoded off with no `Kconfig` exposed for it in this IDF version)
- [ ] Re-enable OTA certificate validation once there's more memory headroom to spare (see "Memory
  constraints & OTA" above)
