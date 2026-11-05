---
title: EEGO Reader A4
nav_order: 17
---

# EEGO Reader A4

The EEGO Reader A4 (sold as the EEGOREAD A4) is an ESP32-S3 reader with a
768x552 panel, a capacitive touchscreen with a touch Home key below the
screen, page-turn buttons, a power button and, on some units, a frontlight.
Its SD card is on an SPI bus, and **File Transfer > USB Drive** exports the
card to a computer over USB.

Support status: the board profile, display driver, touch firmware, battery
gauge and frontlight backend come from the FreeInk SDK, which documents them in
[`freeink-sdk/docs/eego-a4-support.md`](../freeink-sdk/docs/eego-a4-support.md).
The SDK describes its hardware validation as incomplete, and this fork's build
has not been run on an A4. Treat the first install as a test: keep a USB cable
handy and read the serial log if something does not come up.

## Screen

The A4's bezel covers more of the panel than the Xteink readers' and its
corners are rounded. The firmware starts with the SDK's measured allowances
(24 px top, 16 px bottom, 8 px each side); adjust them in **Settings > Display
> Screen Calibration** if text or status bars still sit under the bezel.
CrossInk does not keep chrome out of the rounded corners, so the ends of the
status bar may be clipped there until the side allowances are increased.

Anti-aliased text and images use the panel's three-level and four-level
grayscale modes through the SDK's driver. This is the first CrossInk panel
whose grayscale base is folded into the gray pass, so page turns with
anti-aliasing on are the first thing to check on hardware.

## Buttons, touch and frontlight

- The up and down buttons turn pages; the power button sleeps and wakes.
- A short press of the touch Home key goes Back and a long press goes Home,
  like the X4 Pro's key; the reader's Home-key settings apply.
- The frontlight controls appear only when the light is detected at boot, so
  units without one simply do not show them.

## Installation and updates

Inky (CrossInk's browser installer) does not list this device, so the first
install is done with [esptool](https://docs.espressif.com/projects/esptool/)
over the USB-C port, using the full-flash image from the release:

```sh
esptool --chip esp32s3 --port <port> --baud 921600 write-flash 0x0 firmware-eego-a4-full.bin
```

The full image carries the bootloader, partition table and application, so
it also replaces the stock firmware; back that up first with
`esptool read-flash` if you may want it back. The stock firmware is itself a
CrossPoint build, but its update mechanism has not been tested with this
fork's images, so do not rely on it for the first install.

Once CrossInk is running, later releases install like any other board: copy
`firmware-eego-a4.bin` to the SD card and use **Settings > System > SD Card
Firmware Update**. **Check for Updates** looks for this fork's device type on
CrossInk's own release page, which does not publish one, so it reports no
update.
