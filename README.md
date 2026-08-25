# ImageJockey

A LILYGO T-Dongle-S3 variable-media-drive firmware.

Firmware for a [LILYGO T-Dongle-S3](https://github.com/Xinyuan-LilyGO/T-Dongle-S3) that turns it into a
switchable USB boot-media dongle: multiple ISO/IMG files live on its microSD card, and whichever one you
pick (via the on-device button+screen, or over WiFi) gets presented to whatever host it's plugged into as
a raw USB disk — the same trick as `dd`-ing a hybrid ISO onto a USB stick, just switchable in software,
with write support for images you want to persist state on (e.g. a blank writable disk).

Covers hybrid-ISO media (Junos installers, SystemRescue, Ubuntu live/install, and similar) and plain
raw `.img` disks. Not a true SCSI CD-ROM emulator — see "Known limitations" below.

## Using it

**On the device:** the screen shows a 3-row carousel, centered on your browse cursor. Short-press the
BOOT button to cycle through images; long-press to mount the centered one (marked with `* `) as the
active USB disk — this triggers a soft USB re-enumerate, so the host sees it without a physical unplug.
The last entry, `+ NEW IMAGE`, lets you create a blank writable `.img` file at a size you pick (short-press
cycles presets, long-press confirms/cancels) — mount it, then format it from the host OS (Windows/macOS/
Linux will all offer to format an unrecognized raw disk; that's expected and fine for a blank image).

Extension decides writability: `.img` files are writable over USB (so a persistent-live-OS or scratch
disk actually keeps state across reboots/hosts), `.iso` files stay read-only (protects install media from
accidental corruption).

**Over WiFi:** the device always runs a SoftAP (`ImageJockey-XXXX`, open, no password) at `192.168.4.1`,
and additionally joins a saved network if you've configured one. It presents the DHCP hostname
`imagejockey` there — whether that resolves as `imagejockey` or `imagejockey.<yourdomain>` depends on
your router (most consumer routers register it; no mDNS responder is set up, so don't count on
`imagejockey.local` specifically). Otherwise just check your router's client list for its IP. Either way,
browse to the device's address for a page that lists images (tap to mount), lets you upload new ones
(drag a file in, streamed straight to the SD card), set WiFi credentials, and check for firmware updates.

**Reflashing/managing images without removing the SD card:** just use the WiFi upload page above —
there's no separate USB drag-and-drop mode (that was considered but WiFi upload covers the same need with
less firmware complexity).

## Hardware

| Function | GPIOs |
|---|---|
| TFT (ST7735, SPI2) | MOSI=3, SCLK=5, CS=4, DC=2, RST=1, Backlight=38 (active-LOW) |
| microSD (SDMMC, 4-bit) | CLK=12, CMD=16, D0=14, D1=17, D2=21, D3=18 |
| Button | GPIO0 (the board's only button — shared with the BOOT/download-mode strap pin) |

16MB flash, PSRAM presence unconfirmed on the base board (not required — firmware doesn't depend on it).

**Important:** this board has a single native USB port doing double duty as both the flashing/console
port and the firmware's own USB device (MSC). Once firmware installs the TinyUSB MSC driver, normal
flashing over that port stops working — reflash by holding **BOOT while plugging the board in**, which
forces the ROM bootloader instead of booting the app. Holding BOOT for the first ~50ms of a normal boot
(no replug needed, just hold it before/through power-up) instead skips MSC entirely for that boot, keeping
the USB-Serial/JTAG console alive — useful for debugging, since console and MSC can't be up at the same
time (they share the same physical D+/D- pins).

## Toolchain: WSL2 + Docker

No native ESP-IDF install, on Windows or in WSL — everything builds inside the pinned
[`espressif/idf:v5.5.5`](https://hub.docker.com/r/espressif/idf) image via `docker/Dockerfile` and
`docker-compose.yml`. Run from a WSL shell (Ubuntu), in this project directory
(`/mnt/d/Owncloud.cl/Electronics/TDisplay-Media` from WSL, or wherever you cloned it).

```bash
./scripts/build.sh          # idf.py build
./scripts/menuconfig.sh     # idf.py menuconfig (interactive)
./scripts/fullclean.sh      # idf.py fullclean
./scripts/shell.sh          # drop into the container with the IDF env sourced
```

## Flashing

**Native Windows flashing is the reliable path** — `usbipd-win`-based flashing from WSL (passing the
board's USB port through into WSL, `./scripts/flash.sh` etc.) works for *building*, but was unreliable for
the flash step itself in practice (USB/IP corruption mid-transfer, lost auto-reset-to-bootloader signals).
Flash directly from Windows instead, using the `.bin` files the Docker build already produced on the
shared filesystem:

```powershell
# One-time setup: a venv with esptool, anywhere convenient
python -m venv D:\temp\claude\tdongle-flash-venv
D:\temp\claude\tdongle-flash-venv\Scripts\pip.exe install esptool

# Each flash (find the board's COM port in Device Manager if not COM14).
# Note the partition table is OTA-capable (two app slots + otadata), so this
# needs an extra write compared to a single-factory-partition layout:
D:\temp\claude\tdongle-flash-venv\Scripts\esptool.exe --chip esp32s3 -p COM14 -b 460800 `
  --before default-reset --after hard-reset write-flash `
  --flash-mode dio --flash-size 16MB --flash-freq 80m `
  0x0 build/bootloader/bootloader.bin `
  0x8000 build/partition_table/partition-table.bin `
  0xf000 build/ota_data_initial.bin `
  0x20000 build/imagejockey.bin
```

If the board isn't in download mode (fresh MSC firmware running, or first flash ever), do the BOOT-hold
dance first: unplug, hold BOOT, plug in, wait ~2s, release, then flash.

If you'd rather stay fully in WSL and accept the occasional retry, `./scripts/flash.sh` after
`usbipd attach`ing the board works too — see that script's header comment for the attach commands.

## Firmware updates (OTA)

Once a GitHub release exists with an `imagejockey.bin` asset attached, the device can update itself:
connect it to real WiFi (not just its own SoftAP — needs internet access) and hit "Check for update" on
the web page. It downloads `.../releases/latest/download/imagejockey.bin`, verifies it, and reboots
into it. Auto-rollback (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`) reverts to the previous working image if
the new one never gets far enough to boot cleanly.

To publish a release from WSL: `./scripts/release.sh v0.3.0` (builds via Docker, tags, and pushes a
GitHub release with the binary attached via `gh`). Requires `gh auth login` once.

## Known v1 limitations

- FAT32's 4GiB−1 file size ceiling applies to images on the SD card — fine for Junos/SystemRescue/Ubuntu
  Server/most Linux live images, too small for a full Ubuntu Desktop ISO (~5-6GB) or a 4GB+ writable disk.
- No true CD-ROM/El Torito emulation — raw/hybrid-ISO disk mode only.
- The WiFi credentials form doesn't validate the network actually works before rebooting into it — if it
  fails to connect, the SoftAP is still up as a fallback, so the device stays reachable either way.

## Project layout

- `main/` — firmware source. `msc_disk.c` (raw TinyUSB MSC callbacks), `media.c` (the shared,
  mutex-guarded "which image is mounted" state used by both the USB and WiFi/UI sides), `display.c`/
  `sdcard.c`/`button.c` (board drivers), `ui.c` (the on-device carousel), `wifi.c`/`web.c`/`ota.c` (WiFi
  management page + OTA).
- `components/esp_tinyusb_core/` — a trimmed **local fork** of `espressif/esp_tinyusb`, with its
  storage-wrapper files removed. That component's own `tud_msc_*` implementations are compiled in
  unconditionally whenever its `CONFIG_TINYUSB_MSC_ENABLED` Kconfig flag is on — which is also the *only*
  way to get the underlying TinyUSB MSC class code — so there's no way to use its convenient
  `tinyusb_driver_install()` alongside our own file-backed-disk callbacks without this fork. See the
  comment at the top of that component's `CMakeLists.txt`.
- `docker/`, `docker-compose.yml`, `scripts/` — the WSL+Docker build/flash toolchain.
- `partitions.csv` — OTA-capable layout (two 2MB app slots + otadata), not the default single-factory-app
  table.

## Phase status

- [x] Phase 0 — project scaffold + Docker toolchain
- [x] Phase 1 — display + SD + button bring-up, standalone image browsing
- [x] Phase 2 — USB MSC boot mode, including writable `.img` support and on-device blank-image creation
- [ ] ~~Phase 3 — USB drag-and-drop file-transfer mode~~ — superseded by WiFi upload (below), not built
- [x] Phase 4 — WiFi provisioning + web upload/selection + OTA
- [ ] Phase 5 — polish (on-screen WiFi/mode status, low-SD-space handling, more error states)
