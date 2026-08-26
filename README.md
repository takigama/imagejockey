# ImageJockey

Firmware for a [LILYGO T-Dongle-S3](https://github.com/Xinyuan-LilyGO/T-Dongle-S3) that turns it into a
switchable USB boot-media dongle: multiple ISO/IMG files live on its microSD card, and whichever one you
pick (via the on-device button+screen, or over WiFi) gets presented to whatever host it's plugged into as
a raw USB disk — the same trick as `dd`-ing a hybrid ISO onto a USB stick, just switchable in software,
with write support for images you want to persist state on (e.g. a blank writable disk).

Covers hybrid-ISO media (Junos installers, SystemRescue, Ubuntu live/install, and similar) and plain
raw `.img` disks. Not a true SCSI CD-ROM emulator. Its not the fastest thing in the world and hopefully
if someone makes another t-dongle with a chip that has a faster usb (esp32's are 12mb/s max), it could
actually work quite well. As it stands, takes about 8 minutes to boot a copy of systemrescuecd.

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

**Reflashing/managing images without removing the SD card:** either use the WiFi upload page above, or
hit "Enable SD passthrough" on it — that exposes the *whole* SD card raw over USB, like a normal card
reader, instead of just the currently-mounted image. Good for large files or when WiFi upload would be
too slow. While passthrough is on, the device isn't presenting as a bootable image drive; disable it
(same page) to go back to normal operation once you're done copying files.

## Installing

**First time**, pick one:

- **From a browser** — [flash it from this page](https://takigama.github.io/imagejockey/) (Chrome or
  Edge only — needs the Web Serial API). No software to install; just plug the board in, put it in
  download mode, and click a button.
- **From the command line** — download the four files attached to the
  [latest release](https://github.com/takigama/imagejockey/releases/latest) and flash them with
  `esptool` — see [build.md](build.md) for the exact command, and for why it's four files instead of
  one.

**After that:** updates don't need a cable or the browser flasher. On the device's web page, hit "Check
for update" (works fine with the drive still plugged in and in use) and, if one's available, "Update
now" — the device briefly reboots without presenting as a USB drive to install it, then comes back
normal. Requires the device to be joined to real WiFi with internet access (its own SoftAP alone won't
reach GitHub).

## Known limitations

- FAT32's 4GiB−1 file size ceiling applies to images on the SD card — fine for Junos/SystemRescue/Ubuntu
  Server/most Linux live images, too small for a full Ubuntu Desktop ISO (~5-6GB) or a 4GB+ writable disk.
  A 64GB+ card works fine for total capacity, but only if formatted FAT32 — most ship pre-formatted
  exFAT, which isn't supported yet.
- No true CD-ROM/El Torito emulation — raw/hybrid-ISO disk mode only.
- The WiFi credentials form doesn't validate the network actually works before rebooting into it — if it
  fails to connect, the SoftAP is still up as a fallback, so the device stays reachable either way.
- Firmware update downloads use HTTPS but don't currently validate GitHub's certificate (still encrypted,
  just not authenticated) — see [code.md](code.md) for why.

For firmware internals, see [code.md](code.md). For building from source, see [build.md](build.md).
