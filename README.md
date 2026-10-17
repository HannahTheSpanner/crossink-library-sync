# CrossInk Library Sync

This is my personal fork of [CrossInk](https://github.com/uxjulia/crossink), my preferred of the popular forks of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader), the open firmware for Xteink e-readers.

It is CrossInk v1.6.1 with one feature added, bulk syncing of an OPDS library, plus the fixes that feature needed on the X3 and X4. Everything else (fonts, themes, reading stats, controls and so on) is CrossInk's work and behaves exactly as it does upstream. See the [CrossInk repository](https://github.com/uxjulia/crossink) for those features.

## What this fork changes

### 1. Sync missing books from an OPDS catalogue

CrossInk's OPDS browser downloads one book at a time. This fork adds a **Sync missing books** row that compares a whole catalogue with the books on the SD card and downloads everything that is missing, in one go.

**Where it appears**

- At a server's top level, when the server has an "All Books" (or "Catalog") entry. Grimmory and BookOrbit both have one.
- At the top of any catalogue that lists books, such as a shelf, series or search result. It syncs the whole catalogue from its first page, even if you are viewing a later page.

**How a sync runs**

1. **Check the device.** The card is scanned for EPUBs, up to six folders deep. A catalogue book counts as present when any of the following is true:
   - A local file has a name a downloader would give it: `Author - Title`, `Title - Author`, `Last, First - Title`, `Title, The - Author`, the bare title, or the server's own file name. Names are compared without regard to case, accents or punctuation.
   - CrossInk's Library index has an EPUB with the same title and author.
   - An earlier sync downloaded it from the same server, even if it has since been renamed on the server.
2. **Check the catalogue.** Every page is fetched by following its "next" links, up to 200 pages, with loop detection. If any page still can't be read after three attempts, the sync stops before downloading anything, so a partial catalogue is never treated as complete. Entries without an EPUB link (PDF-only books, for example) are counted and skipped.
3. **Confirm, then download.** You see how many books are missing before anything downloads. Books are saved one at a time to the OPDS download folder, using the server's naming format. One failed book doesn't stop the rest, and you can cancel at any point.

Nothing is ever replaced or deleted.

**How it fits in the memory**

The X3 and X4 have about 380 KB of RAM and no PSRAM, so the sync is designed so its memory use stays flat however large the library is:

- Catalogue pages are parsed as a stream, one entry at a time, instead of being loaded whole.
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
- **Patch wolfSSL at build time.** `scripts/patch_wolfssl.py` modifies wolfSSL's input-buffer code during the build, so the receive buffer can use the reserved block.
  - It also reports whether the server accepted small records.
  - The firmware build fails if the patch didn't apply, so a build can't ship without the fix.
- **Tests.** Host tests cover the reserve (`test/tls_record_reserve`).

### 3. Library sync memory on the X3 and X4

**Problem.** With the HTTPS fix in place, single downloads worked, but a sync still failed. From the server's top level it stopped on the first catalogue page with "Failed to fetch feed". Started from a 50-book list, it sat on "Page 0" indefinitely.

**Cause.** The serial log showed two separate memory shortfalls while a sync's HTTPS connection was open:
- **The catalogue parser.** It fed the XML parser (expat) 1 KB at a time. Partway through a page, that overflowed expat's 2 KB working buffer, so expat tried to grow it to 4 KB. That needs a free 4 KB block, and there wasn't one.
- **The book list behind the sync screen.** The browser kept its loaded page (up to 50 books, about 6 to 13 KB) in memory for the whole sync. From a 50-book list that left the reader with as little as 3.7 KB free. At that level WiFi can't even receive data, so the TLS handshake stalled.

**Fix.**
- **Smaller parser slices.** The parser now feeds expat 512 bytes at a time, so expat's buffer never needs to grow. A host test (`test/opds_parser_memory`) checks this:
  - It builds the firmware's own copy of expat with the device's settings.
  - It records every allocation made while parsing a realistic 50-book Grimmory page.
  - It fails if any single request exceeds 2 KB.
- **Free the browser's page during a sync.** Starting a sync frees the browser's page, and the page reloads when the sync screen closes.
- **No unused space in the book list.** The sync's list of books on the card starts small and gives back unused space once it's built. This matters more as a library and its sync history grow.
- **Retry and report memory failures.** A page that runs out of memory is retried. If it still fails, the error says "Memory error" instead of "Failed to fetch feed".
- **Heap logging.** The serial log records free memory before each catalogue page (`[OSY] Page N attempt M: heap free=... largest=...`).

### 4. OPDS reliability

- Brief connection failures are retried automatically: catalogue pages up to three times, downloads twice, with a short back-off. Errors that won't change on retry, such as a rejected login or a missing page, are not retried.
- OPDS errors show the server's HTTP status, for example "Failed to fetch feed (HTTP 401)" for a rejected username or password.
- **Retry** after a failed download retries that book instead of reloading the catalogue page, and **Back** returns to the book list.

## Credit: Library Sync for KOReader

The sync feature is a port of the "Sync missing books" flow from [Library Sync for KOReader](https://github.com/komadorirobin/library-sync.koplugin) by Robin Bolinsson (komadorirobin), released under the MIT licence. All credit for the idea and the workflow goes to that project. If you use KOReader, it is the plugin to use, and it does far more than this port.

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

Download from [Releases](https://github.com/HannahTheSpanner/crossink-library-sync/releases). To install, copy the file to the SD card, then on the device open **Settings > System > SD Card Firmware Update** and select it. USB flashing is covered in [docs/installation.md](./docs/installation.md).

Installing an official CrossInk update over the air replaces this fork with upstream CrossInk.

## Building

GitHub Actions builds all three firmware files on every push, and runs the OPDS and TLS host tests. To build locally, use PlatformIO as described in CrossInk's [Getting Started](./docs/development/getting-started.md) guide:

```sh
git submodule update --init --recursive
pio run -e default      # X3 and X4
pio run -e x4-classic   # X4 Classic
pio run -e x4-pro       # X4 Pro
```

Host tests for this fork's changes:

```sh
cmake -S test -B build-tests
cmake --build build-tests --target OpdsParserTest OpdsParserMemoryTest OpdsLibraryMatchTest TlsRecordReserveTest
ctest --test-dir build-tests -R "Opds|TlsRecordReserve"
```

## Upstream and licence

- [CrossInk](https://github.com/uxjulia/crossink) is by uxjulia, and almost all of this firmware is her work and the work of CrossPoint's contributors. Please report problems with anything other than library sync and the OPDS fixes above to CrossInk, not here. If you'd like to support CrossInk, uxjulia has a [Ko-fi](https://ko-fi.com/Q5Q01M6S7).
- [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) is the firmware CrossInk is built on.
- [Library Sync for KOReader](https://github.com/komadorirobin/library-sync.koplugin) is the plugin the sync feature is ported from.

This fork is released under the same MIT licence as CrossPoint and CrossInk; see [LICENSE](./LICENSE).
