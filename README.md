# CrossInk Library Sync

This is my personal fork of [CrossInk](https://github.com/uxjulia/crossink), my preferred of the popular forks of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader), the open firmware for Xteink e-readers.

It is CrossInk v2.0.0 with one feature added, bulk syncing of an OPDS library, plus the fixes that feature needed on the X3 and X4, and one extra reader font, EB Garamond. It builds for every device CrossInk supports: the Xteink X3, X4, X4 Classic and X4 Pro, the Seeed reTerminal Sticky and the BOOX Picco, plus two readers that CrossPoint supports and CrossInk does not yet: the Metalio E-Ink 4 and the EEGO Reader A4. Everything else is the work of the CrossPoint team and uxjulia, and behaves exactly as it does upstream: the reader itself, the other fonts, themes, reading stats, controls and so on. See [Credits and support](#credits-and-support) for links to both projects.

> **A note on credit.** I take no credit for the work in this fork. Claude, Anthropic’s AI model, wrote the code, chased down the bugs and wrote these docs. My role was project management in the loosest possible sense: describing what I wanted, plugging in a USB cable, pasting serial logs, and announcing “it still doesn’t work” with the confidence of someone who could not have fixed it herself.
>
> Everything here sits on firmware that people designed, wrote, debugged and still maintain by hand, for a device with a few hundred kilobytes of memory. That takes real skill and a lot of hard work, and none of this fork would exist without it. If this fork is useful to you, please support the humans who did that work: the authors of CrossPoint and CrossInk, listed under [Credits and support](https://github.com/HannahTheSpanner/crossink-library-sync#credits-and-support).

## What this fork changes

### 1. Sync missing books from an OPDS catalogue

CrossInk's OPDS browser (in **File Transfer**) downloads one book at a time. This fork adds a **Sync missing books** row that compares a whole catalogue with the books on the SD card and downloads everything that is missing, in one go. It works with OPDS 1 (Atom) and OPDS 2 (JSON) catalogues.

This would not be possible without the fantastic [Library Sync plugin for KOReader](https://github.com/komadorirobin/library-sync.koplugin) by Robin Bolinsson. The feature is a port of its "Sync missing books" flow; see [Credit: Library Sync for KOReader](#credit-library-sync-for-koreader) for how it was ported.

**Where it appears**

- At a server's top level, when the server has an "All Books" (or "Catalog") entry. Grimmory and BookOrbit both have one.
- At the top of any catalogue that lists books, such as a shelf, series or search result. It syncs the whole catalogue from its first page, even if you are viewing a later page.
- An OPDS 2 page whose books are only previews of its groups offers its "All Books" catalogue instead, if it has one.

**How a sync runs**

1. **Check the device.** The card is scanned for EPUBs, up to six folders deep. A catalogue book counts as present when any of the following is true:
   - A local file has a name a downloader would give it: `Author - Title`, `Title - Author`, `Last, First - Title`, `Title, The - Author`, the bare title, or the server's own file name. Names are compared without regard to case, accents or punctuation.
   - CrossInk's Library index has an EPUB with the same title and author.
   - An earlier sync downloaded it from the same server, even if it has since been renamed on the server.
2. **Check the catalogue.** Every page is fetched by following its "next" links, up to 200 pages, with loop detection. If any page still can't be read after three attempts, the sync stops before downloading anything, so a partial catalogue is never treated as complete. Entries without an EPUB link (PDF-only books, or OPDS 2 books that must be bought or need LCP) are counted and skipped.
3. **Confirm, then download.** You see how many books are missing before anything downloads. Books are saved one at a time exactly as the OPDS browser would save them: to the server's Download Folder, with its Filename format, or under the server's own file name when **Use Server Filename** is on. OPDS 2 books that link to a publication page rather than the EPUB are resolved first. One failed book doesn't stop the rest, and you can cancel at any point.

Nothing is ever replaced or deleted. With Use Server Filename on, a book whose server file name is already on the card counts as present.

**How it fits in the memory**

The X3 and X4 have about 380 KB of RAM and no PSRAM, so the sync is designed so its memory use stays flat however large the library is:

- Catalogue pages are parsed as a stream, one entry at a time, instead of being loaded whole. Both the Atom and the JSON parser have a streaming mode for this.
- The queue of missing books is written to the SD card (`/.crosspoint/opds_sync_queue.tsv`), not held in memory.
- Books on the card are held as a sorted array of 8-byte hashes of their normalised names, so checking a catalogue book is a binary search.
- Each server has a small ledger on the card (`/.crosspoint/opds_sync_<hash>.txt`) recording the OPDS identity of every book it synced. That is how renamed books are still recognised.

The full user guide is in [Syncing missing books](./docs/user-guide.md#syncing-missing-books).

### 2. OPDS over HTTPS on the X3 and X4

**Problem.** On the X3 and X4, small OPDS pages loaded, but book lists and downloads from an HTTPS server usually failed with "Failed to fetch feed" or "Download failed". The same server worked every time from KOReader.

**Cause.**
- HTTPS (TLS) sends data in records of up to 16 KB, and the reader has to hold a whole record in one contiguous block of memory (about 16.7 KB) to decrypt it.
- Once WiFi is running on an X3 or X4, the heap rarely has a free block that large. A serial log showed wolfSSL failing with `MEMORY_E` (-125) partway through a response.
- The X4 Classic and X4 Pro have PSRAM, so they never hit this.

**Fix.**
- **Ask for small records.** The reader asks the server for 2 KB records using the TLS max_fragment_length extension (RFC 6066). If the server agrees, no large buffer is ever needed.
- **Reserve a buffer for servers that refuse.** Many servers ignore the request. For those, the OPDS browser sets aside one 16.7 KB block as soon as it opens, before WiFi starts and while a block that size is still free.
  - wolfSSL's allocator is routed through hooks, so its receive buffer borrows that block instead of asking the heap for one.
  - If the server accepts small records, the block is handed straight back.
  - Boards with PSRAM skip all of this.
  - CrossInk v2.0.0 added its own fix for downloads: once a book's request is sent, the connection allocates one full-record receive buffer and keeps it for the whole transfer. On the X3 and X4 that allocation now takes the block set aside at the start, which is still free when the heap no longer has one that large. Catalogue pages, which v2.0.0 does not cover, use the block the same way.
- **Patch wolfSSL at build time.** `scripts/patch_wolfssl.py` modifies wolfSSL's input-buffer code during the build, so the receive buffer can use the reserved block.
  - It also reports whether the server accepted small records.
  - It composes with v2.0.0's own wolfSSL patches, including on a dependency cache patched by an older build of this fork.
  - The firmware build fails if the patch didn't apply, so a build can't ship without the fix.
- **Tests.** Host tests cover the reserve (`test/tls_record_reserve`).

### 3. Library sync memory on the X3 and X4

**Problem.** With the HTTPS fix in place, single downloads worked, but a sync still failed. From the server's top level it stopped on the first catalogue page with "Failed to fetch feed". Started from a 50-book list, it sat on "Page 0" indefinitely.

**Cause.** The serial log showed two separate memory shortfalls while a sync's HTTPS connection was open:
- **The catalogue parser.** It fed the XML parser (expat) 1 KB at a time. Partway through a page, that overflowed expat's 2 KB working buffer, so expat tried to grow it to 4 KB. That needs a free 4 KB block, and there wasn't one.
- **The book list behind the sync screen.** The browser kept its loaded page (up to 50 books, about 6 to 13 KB) in memory for the whole sync. From a 50-book list that left the reader with as little as 3.7 KB free. At that level WiFi can't even receive data, so the TLS handshake stalled.

**Fix.**
- **No parser buffer growth.** Feeding expat 512 bytes at a time kept its buffer at 2 KB. CrossInk v2.0.0 turned off expat's input context (`XML_CONTEXT_BYTES=0`), which keeps CrossInk's own 1 KB slices within 2 KB too, so this fork now uses them. A host test (`test/opds_parser_memory`) checks this:
  - It builds the firmware's own copy of expat with the device's settings.
  - It records every allocation made while parsing a realistic 50-book Grimmory page.
  - It fails if any single request exceeds 2 KB (and does fail with the old expat setting).
- **Free the browser's page during a sync.** Starting a sync frees the browser's page, and the page reloads when the sync screen closes.
- **No unused space in the book list.** The sync's list of books on the card starts small and gives back unused space once it's built. This matters more as a library and its sync history grow.
- **Retry and report memory failures.** A page that runs out of memory is retried. If it still fails, the error says "Memory error" instead of "Failed to fetch feed".
- **Heap logging.** The serial log records free memory before each catalogue page (`[OSY] Page N attempt M: heap free=... largest=...`).

### 4. OPDS reliability

- Brief connection failures and server errors (408, 429, 5xx) are retried automatically: catalogue pages up to three times, downloads twice, with a short back-off. CrossInk v2.0.0 already resumes a download that is cut short; these retries cover server errors and fresh starts. Errors that won't change on retry, such as a rejected login or a missing page, are not retried.
- Catalogue errors use CrossInk v2.0.0's messages (login rejected, server error, unreachable). Download errors add the server's HTTP status, for example "Download failed (HTTP 404)".
- **Retry** after a failed download retries that book instead of reloading the catalogue page, and **Back** returns to the book list.

### 5. EB Garamond reader font

[EB Garamond](https://github.com/octaviopardo/EBGaramond12) by Georg Duffner and Octavio Pardo, a classic old-style serif, is added as a reader font. It is released under the SIL Open Font License 1.1, which is included next to the font files.

- **X4 Classic, X4 Pro, Sticky, Picco, Metalio E-Ink 4 and EEGO Reader A4:** built in, next to Lexend Deca and Bitter, as a scalable font from 8 to 22 pt. Choose it in **Settings > Reader > Font Options > Font Family** or the reader's font menu.
- **X3 and X4:** not built in. These readers have no scalable fonts, and EB Garamond's pre-rendered sizes do not fit in their firmware partition next to CrossInk v2.0.0. Each release instead includes `EB-Garamond-SD-font.zip`: unzip it and copy the `EB Garamond` folder to `/.fonts/` on the SD card. It then appears in the font list like any SD font, in sizes from 8 to 20 pt.

Regular and italic text use EB Garamond's Medium weight, which holds up better on e-ink than the thinner Regular (Bitter's built-in copy does the same). Both copies cover Latin, Greek and Cyrillic.

### 6. Metalio E-Ink 4 and EEGO Reader A4

CrossPoint builds for these two ESP32-S3 readers; CrossInk does not. The hardware support (board profile, display, touch, battery, power) lives in the FreeInk SDK that CrossInk already pins, so this fork adds the build environments and the small amount of app-side behaviour CrossPoint has for them:

- **Metalio:** page buttons behind an I2C expander (polled, as they raise no GPIO interrupt), Picco-style touch gestures, power-off instead of deep sleep (the board cannot sleep with its modem powered), and the panel's full clearing waveform for sleep images.
- **EEGO A4:** the bezel allowances measured by the SDK as the Screen Calibration default, the frontlight turned off and the touch controller parked before deep sleep, and USB Drive over the SPI SD card.

Both get a `-full.bin` image in each release for a first install with esptool, since no browser installer lists them. The SDK marks both boards' hardware validation as incomplete, and this fork has not been run on either device, so treat them as test builds. See [docs/metalio-eink4.md](./docs/metalio-eink4.md) and [docs/eego-a4.md](./docs/eego-a4.md).

### Reading a crash report

After a crash the reader writes `crash_report.txt` to the SD card root with a raw backtrace. To turn it into function names and line numbers, create a branch named `debug/<anything>` from the **released tag** the device was running, put the environment and the addresses in `.github/symbolize/request.txt`, and push. The `Symbolize crash` workflow rebuilds that commit and prints the symbolised trace in its run summary. (Or run the workflow by hand from the Actions tab with the environment and addresses as inputs.) The summary also prints the ELF SHA-256; its first digits should match the report's "Firmware ELF SHA256" line. Frames inside Espressif's prebuilt libraries (lwIP, Wi-Fi) have no symbols and show as the nearest fork function, so read those from the "Panic reason" line instead.

## Credit: Library Sync for KOReader

This feature would not be possible without the fantastic [Library Sync for KOReader](https://github.com/komadorirobin/library-sync.koplugin) by Robin Bolinsson ([komadorirobin](https://github.com/komadorirobin)), released under the MIT licence. The sync feature is a port of its "Sync missing books" flow, and all credit for the idea and the workflow goes to that project. If you use KOReader, it is the plugin to use, and it does far more than this port.

**How it was ported.** No code was copied: the plugin is Lua running inside KOReader, and this fork is C++ firmware for a microcontroller. What carried over is the plugin's design:

- The same three-step flow: scan local books, walk the paginated OPDS catalogue with Basic authentication, then download what is missing.
- The same fuzzy duplicate detection: compare normalised titles and authors across the file-naming patterns downloaders use.
- The same rule of stopping before any download if the catalogue can't be read completely.

**What changed in the port**

KOReader devices have hundreds of megabytes of RAM, so the plugin can hold a whole catalogue in memory. An X3 or X4 cannot. The port therefore:
- streams each page through the parser
- keeps the download queue on the SD card
- matches local books by hash
- adds a per-server ledger of synced books, so a book renamed on the server isn't downloaded twice

**Not ported**
- Mirror mode (moving books removed from the server to trash)
- Metadata refresh and replacement of existing files
- CBZ comics
- The Grimmory and BookOrbit API extras (genres, tags, author images, uploads)
- The plugin's own OTA updates

## Devices and downloads

| Device | Firmware file |
| --- | --- |
| Xteink X3 and X4 | `firmware-x3-x4.bin` |
| Xteink X4 Classic | `firmware-x4-classic.bin` |
| Xteink X4 Pro | `firmware-x4-pro.bin` |
| Seeed reTerminal Sticky | `firmware-sticky.bin` |
| BOOX Picco | `firmware-picco.bin` |
| Metalio E-Ink 4 | `firmware-metalio-eink4.bin` (`-full.bin` for a first install) |
| EEGO Reader A4 | `firmware-eego-a4.bin` (`-full.bin` for a first install) |

Download from [Releases](https://github.com/HannahTheSpanner/crossink-library-sync/releases). To install on an Xteink or the Sticky, copy the file to the SD card, then on the device open **Settings > System > SD Card Firmware Update** and select it. USB flashing is covered in [docs/installation.md](./docs/installation.md).

The BOOX Picco is different: `firmware-picco.bin` is an image for its `app` slot only, and it must be installed with a USB installer that explicitly supports the Picco, without erasing the device, so its recovery software stays intact. SD-card and Wi-Fi updates are not available on the Picco. See [docs/picco.md](./docs/picco.md).

The Metalio E-Ink 4 and EEGO Reader A4 have no browser installer: write the `-full.bin` image with esptool for a first install (`esptool --chip esp32s3 --port <port> write-flash 0x0 firmware-<device>-full.bin`), after which SD-card updates work. See [docs/metalio-eink4.md](./docs/metalio-eink4.md) and [docs/eego-a4.md](./docs/eego-a4.md).

Installing an official CrossInk update over the air replaces this fork with upstream CrossInk.

## Building

GitHub Actions builds all seven firmware files on every push, and runs the OPDS, TLS and download host tests. To build locally, use PlatformIO as described in CrossInk's [Getting Started](./docs/development/getting-started.md) guide:

```sh
git submodule update --init --recursive
pio run -e default      # X3 and X4
pio run -e x4-classic   # X4 Classic
pio run -e x4-pro       # X4 Pro
pio run -e sticky       # reTerminal Sticky
pio run -e picco        # BOOX Picco
pio run -e metalio-eink4  # Metalio E-Ink 4 (also writes firmware-metalio-eink4-full.bin)
pio run -e eego-a4        # EEGO Reader A4 (also writes firmware-eego-a4-full.bin)
```

Host tests for this fork's changes:

```sh
cmake -S test -B build-tests
cmake --build build-tests --target OpdsParserTest OpdsParserMemoryTest OpdsLibraryMatchTest OpdsSyncQueueTest TlsRecordReserveTest HttpDownloaderTest HttpDownloaderDevicePathTest
ctest --test-dir build-tests -R "Opds|TlsRecordReserve|HttpDownloader"
```

## Credits and support

This fork stands on two firmware projects, and the credit for nearly all of it belongs to them equally. My changes are the sync feature and the OPDS fixes described above.

**CrossPoint Reader** is the open-source firmware for Xteink e-readers that CrossInk is a fork of. Credit goes to [Dave Allie](https://github.com/daveallie), who created it, and the CrossPoint team and community of contributors who build it.
- GitHub: [crosspoint-reader/crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader)
- Support: [fund CrossPoint's contributors on royalty.dev](https://app.royalty.dev/crosspoint-reader/crosspoint-reader). CrossPoint also receives a small share of each X3/X4 Developer Edition sold through [crosspointreader.com](https://crosspointreader.com).

**CrossInk** is uxjulia's fork of CrossPoint, adding the typography, themes, reading stats and reader features this fork is built on.
- GitHub: [uxjulia/crossink](https://github.com/uxjulia/crossink)
- Support: [Ko-fi](https://ko-fi.com/uxjulia) or [GitHub Sponsors](https://github.com/sponsors/uxjulia)

**Library Sync for KOReader** by Robin Bolinsson is the plugin the sync feature is ported from.
- GitHub: [komadorirobin/library-sync.koplugin](https://github.com/komadorirobin/library-sync.koplugin)

Please report problems with anything other than library sync and the OPDS fixes above to CrossInk or CrossPoint, not here.

This fork is released under the same MIT licence as CrossPoint and CrossInk; see [LICENSE](./LICENSE).
