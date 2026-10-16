CrossInk with **OPDS library sync**: download every book in an OPDS catalog that is not already on the device, in one go. Ported from KOReader's [Library Sync plugin](https://github.com/komadorirobin/library-sync.koplugin). See [Syncing missing books](https://github.com/HannahTheSpanner/crossink-library-sync/blob/main/docs/user-guide.md#syncing-missing-books).

## Which file for which device

| Device | File |
| --- | --- |
| Xteink X3 and X4 | `firmware-x3-x4.bin` |
| Xteink X4 Classic | `firmware-x4-classic.bin` |
| Xteink X4 Pro | `firmware-x4-pro.bin` |

## Install

1. Copy the matching `.bin` file to the SD card.
2. On the device, open **Settings > System > SD Card Firmware Update** and select the file.

USB flashing with `esptool` is described in [docs/installation.md](https://github.com/HannahTheSpanner/crossink-library-sync/blob/main/docs/installation.md).

Installing an official CrossInk update over the air replaces this fork.
