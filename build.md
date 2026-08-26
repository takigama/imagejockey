# Building & flashing

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

`idf.py fullclean` (or deleting `build/` and `sdkconfig`) is needed after any `sdkconfig.defaults` or
`CMakeLists.txt` change — Kconfig options and CMake cache variables don't reliably pick up mid-build
otherwise.

## Flashing

**Native Windows flashing is the reliable path** — `usbipd-win`-based flashing from WSL (passing the
board's USB port through into WSL, `./scripts/flash.sh` etc.) works for *building*, but was unreliable for
the flash step itself in practice (USB/IP corruption mid-transfer, lost auto-reset-to-bootloader signals).
Flash directly from Windows instead.

Same command either way — only where the four files come from differs:

- **From a build:** they're already in `build/` (`build/bootloader/bootloader.bin`,
  `build/partition_table/partition-table.bin`, `build/ota_data_initial.bin`, `build/imagejockey.bin`).
- **From a downloaded release:** every [release](https://github.com/takigama/imagejockey/releases) has
  all four attached directly (no `build/` prefix, no subfolders) — just point the paths below at wherever
  you downloaded them.

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
  0x0 bootloader.bin `
  0x8000 partition-table.bin `
  0xf000 ota_data_initial.bin `
  0x20000 imagejockey.bin
```

If the board isn't in download mode (fresh MSC firmware running, or first flash ever), do the BOOT-hold
dance first: unplug, hold BOOT, plug in, wait ~2s, release, then flash.

### Why four files, not one?

Every ESP32 flash is actually multiple separate writes to specific byte offsets in flash — there's no
such thing as "the" firmware image, even for the simplest possible project. At minimum that's always a
**bootloader** (tiny, its only job is finding and jumping to the app) plus a **partition table**
(describes what lives where in flash — app, filesystem, NVS, etc.) plus the **app** itself. Tools that
feel like "just flash one file" — `idf.py flash`, the Arduino IDE, `platformio run -t upload` — are
still doing exactly this multi-region write under the hood; they're just hiding it behind one command by
building the bootloader and partition table for you and remembering the offsets.

The other common way projects genuinely ship as a single file is a **merged binary** — `esptool.py
merge_bin` concatenates the separate pieces (with padding) into one flat image written at offset `0x0`,
purely for distribution convenience. We don't currently do that (see "Cutting a release" below for why
keeping the pieces separate is more useful here), but nothing stops you from merging the four files
yourself with `esptool.py merge_bin` if you'd rather juggle one file locally.

The **fourth** file, `ota_data_initial.bin`, is specific to *this* project rather than something every
ESP32 build needs: our [partition table](../partitions.csv) has **two** app slots (`ota_0`/`ota_1`,
see `code.md`) instead of the single-factory-partition layout most basic tutorials use, so there's a
small state blob (`otadata`, at `0x0f000`) recording which slot is currently valid and should boot. A
project with only one possible app location has nothing to choose between, so it skips this file
entirely — that's the difference you're likely remembering from other ESP32 installs.

If you'd rather stay fully in WSL and accept the occasional retry, `./scripts/flash.sh` after
`usbipd attach`ing the board works too — see that script's header comment for the attach commands.

**Writing to both OTA app slots:** esptool's multi-region `write-flash` has been observed silently
dropping a region when several are combined in one invocation on this board/setup — if you need both
`ota_0` (`0x20000`) and `ota_1` (`0x220000`) populated with the same known-good image (e.g. after
recovering from a bad OTA), flash each region as a **fully separate** `esptool` command rather than
listing both offsets in one call, and read back a byte range afterwards to confirm if in doubt.

**Debug/console mode:** the board's single USB port does double duty as both the flashing/console port
and the firmware's own USB device (MSC) — once MSC installs, the console goes silent (they share the
same physical D+/D− pins, can't both be up). Holding BOOT for the first ~50ms of a *normal* boot (not
the download-mode dance — just hold it through power-up, no replug) skips MSC for that boot instead,
keeping the USB-Serial/JTAG console alive. In practice this is hard to time by hand, since GPIO0 is also
the ROM bootloader's own strapping pin and holding it at power-up usually just wins download mode
instead — see `app_main.c`'s `boot_button_held()` if you want to force it on for a debugging session
(temporarily hardcode `debug_mode = true`, per the comment already there from the last time this was
needed).

## Cutting a release / OTA

Devices on real WiFi can update themselves once a GitHub release exists with an `imagejockey.bin` asset
attached — see the README's "Installing" section for the on-device side of this.

To publish one from WSL:

```bash
./scripts/release.sh v0.3.1 "release notes here"
```

This tags the current commit **before** building (`main/ota.c`'s own idea of its version comes from
`git describe` at build time — see top-level `CMakeLists.txt` — so the tag has to exist first, or the
binary embeds "N commits past the previous tag" instead of the tag it's actually released under, and
the device's update-check never reports itself as up to date). Then it builds via Docker, publishes a
GitHub release with all four flash images attached via `gh`, and finally copies those same four files
into `docs/` (overwriting whatever was there), bumps `docs/manifest.json`'s version, and pushes that as
a follow-up commit — that's what keeps the [browser flasher](https://takigama.github.io/imagejockey/)
in sync with the latest release. Requires `gh auth login` once beforehand.

The web flasher itself is just `docs/index.html` + `docs/manifest.json` + those four `.bin` files,
served by GitHub Pages (Settings → Pages → source: `master` branch, `/docs` folder). It uses
[ESP Web Tools](https://esphome.github.io/esp-web-tools/) — the four files are checked into `docs/`
rather than fetched live from Releases because GitHub's release-asset URLs aren't reliably
CORS-enabled, which the in-browser flashing needs.
