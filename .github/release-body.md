CrossInk with **OPDS library sync**: download every book in an OPDS catalog that is not already on the device, in one go. Ported from KOReader's [Library Sync plugin](https://github.com/komadorirobin/library-sync.koplugin). See [Syncing missing books](https://github.com/HannahTheSpanner/crossink-library-sync/blob/main/docs/user-guide.md#syncing-missing-books).

## Which file for which device

| Device | File |
| --- | --- |
| Xteink X3 and X4 | `firmware-x3-x4.bin` |
| Xteink X4 Classic | `firmware-x4-classic.bin` |
| Xteink X4 Pro | `firmware-x4-pro.bin` |
| Seeed reTerminal Sticky | `firmware-sticky.bin` |
| BOOX Picco | `firmware-picco.bin` |

`EB-Garamond-SD-font.zip` is the EB Garamond reader font for the X3 and X4, which have no room for it built in. Unzip it and copy the `EB Garamond` folder to `/.fonts/` on the SD card. The other devices already have it built in.

## Install

**Xteink and Sticky**

1. Copy the matching `.bin` file to the SD card.
2. On the device, open **Settings > System > SD Card Firmware Update** and select the file.

USB flashing with `esptool` is described in [docs/installation.md](https://github.com/HannahTheSpanner/crossink-library-sync/blob/main/docs/installation.md).

**BOOX Picco**

`firmware-picco.bin` is an application image for the Picco's `app` slot (offset `0xE0000`), not a full flash image. Install it only with a USB installer that explicitly supports the BOOX Picco, and never erase the device or write a factory image: the Picco's recovery software must stay intact. CrossInk's Wi-Fi and SD-card updates are not available on the Picco. See [docs/picco.md](https://github.com/HannahTheSpanner/crossink-library-sync/blob/main/docs/picco.md).

Installing an official CrossInk update over the air replaces this fork.
