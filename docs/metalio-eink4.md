---
title: Metalio E-Ink 4
nav_order: 16
---

# Metalio E-Ink 4

The Metalio E-Ink 4 is a compact ESP32-S3 reader with a 3.97" 800x480 panel
(the X4 Pro's panel class), a capacitive touchscreen, two volume buttons that
turn pages, a BOOT button that acts as Confirm, and a power button. It has no
frontlight. Its SD card is the 1-bit SDMMC slot, and **File Transfer > USB
Drive** exports the card to a computer over USB.

Support status: the board profile, display, touch, battery gauge and power
sequencing come from the FreeInk SDK, which documents them in
[`freeink-sdk/docs/metalio-eink4-support.md`](../freeink-sdk/docs/metalio-eink4-support.md).
The SDK describes that support as compiled and host-tested with physical
hardware validation pending, and this fork's build has not been run on a
Metalio either. Treat the first install as a test: keep a USB cable handy and
read the serial log if something does not come up.

## Buttons and touch

- Volume up and down turn pages; BOOT is Confirm; the power button sleeps and
  wakes. The volume buttons sit behind an I2C expander, so the firmware polls
  them (every 20 ms while you are pressing buttons, every 100 ms after a few quiet seconds) rather than waking on a GPIO edge.
- Touch follows the Picco: swipe up from the bottom edge to open the menu
  (which carries Home), and while reading swipe up anywhere for the Reader
  Menu. There is no Home key.
- The speaker, microphone, vibration motor and accelerometer are not used by
  CrossInk.

## Sleep and power

The Metalio cannot deep-sleep with its modem and audio module still powered,
so **Sleep** turns the board off through its power controller; the power
button cold-boots it, and the saved book reopens as usual. While USB power is
connected the board stays up and sleeps normally instead. Sleep screens use
the panel's full clearing waveform, which avoids the previous page showing
through a four-shade sleep image on this panel.

## Installation and updates

Inky (CrossInk's browser installer) does not list this device, so the first
install is done with [esptool](https://docs.espressif.com/projects/esptool/)
over the USB-C port, using the full-flash image from the release:

```sh
esptool --chip esp32s3 --port <port> --baud 921600 write-flash 0x0 firmware-metalio-eink4-full.bin
```

Hold BOOT while connecting the cable if the port does not appear. The full
image carries the bootloader, partition table and application, so it also
replaces whatever firmware was on the device; back that up first with
`esptool read-flash` if you may want it back.

Once CrossInk is running, later releases install like any other board: copy
`firmware-metalio-eink4.bin` to the SD card and use **Settings > System > SD
Card Firmware Update**. **Check for Updates** looks for this fork's device type
on CrossInk's own release page, which does not publish one, so it reports no
update.
