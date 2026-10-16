#include "OpdsLibrarySyncActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <Logging.h>
#include <Memory.h>
#include <ZipFile.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <utility>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "network/DownloadFileSwap.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"
#include "util/StringUtils.h"
#include "util/UrlUtils.h"

namespace fui = freeink::ui;

namespace {
constexpr char LOG_TAG[] = "OSY";
constexpr fui::ActionId ACTION_CONFIRM = 1;
constexpr fui::ActionId ACTION_CANCEL = 2;
constexpr char QUEUE_PATH[] = "/.crosspoint/opds_sync_queue.tsv";
constexpr char PAGE_PATH[] = "/.crosspoint/opds_sync_page.tsv";
constexpr char LEDGER_TMP_SUFFIX[] = ".tmp";
constexpr size_t DOWNLOAD_BUFFER_SIZE = 2048;
constexpr int DOWNLOAD_PROGRESS_STEP_PERCENT = 5;
constexpr unsigned long DOWNLOAD_PROGRESS_MIN_UPDATE_MS = 5000;
constexpr unsigned long CATALOGUE_PROGRESS_MIN_UPDATE_MS = 3000;
// The low 16 bits of a metadata entry carry the library index ordinal so a
// metadata match can be checked against the card (the index may be stale).
constexpr uint64_t ORDINAL_MASK = 0xFFFFULL;

// Tabs and newlines delimit queue records; nothing else is reserved.
void appendField(std::string& out, const std::string& value) {
  std::transform(value.begin(), value.end(), std::back_inserter(out),
                 [](const char c) { return (c == '\t' || c == '\n' || c == '\r') ? ' ' : c; });
}

void formatKey(const uint64_t key, char (&out)[17]) {
  snprintf(out, sizeof(out), "%08lx%08lx", static_cast<unsigned long>(key >> 32),
           static_cast<unsigned long>(key & 0xFFFFFFFFULL));
}

bool parseKey(const std::string_view text, uint64_t& out) {
  if (text.size() != 16) return false;
  uint64_t value = 0;
  for (const char c : text) {
    int digit = -1;
    if (c >= '0' && c <= '9') digit = c - '0';
    if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
    if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
    if (digit < 0) return false;
    value = (value << 4) | static_cast<uint64_t>(digit);
  }
  out = value;
  return true;
}

// Splits `line` at tabs into at most `count` fields.
size_t splitFields(std::string_view line, std::string_view* fields, const size_t count) {
  size_t found = 0;
  while (found < count) {
    const size_t tab = line.find('\t');
    fields[found++] = line.substr(0, tab);
    if (tab == std::string_view::npos) break;
    line.remove_prefix(tab + 1);
  }
  return found;
}

bool validateEpub(const std::string& path) {
  ZipFile zip(path);
  size_t size = 0;
  return zip.getInflatedFileSize("META-INF/container.xml", &size) && size > 0;
}

#ifdef SIMULATOR
// Deterministic two-page catalogue so the flow can be exercised without a server.
constexpr char SIM_PAGE_1[] = R"(<feed xmlns="http://www.w3.org/2005/Atom">
<link rel="next" href="/sim/all?page=2" type="application/atom+xml"/>
<entry><title>The Left Hand of Darkness</title><id>urn:sim:1</id><author><name>Ursula K. Le Guin</name></author>
<link href="/books/1.epub" rel="http://opds-spec.org/acquisition" type="application/epub+zip"/></entry>
<entry><title>Kindred</title><id>urn:sim:2</id><author><name>Octavia E. Butler</name></author>
<link href="/books/2.epub" rel="http://opds-spec.org/acquisition" type="application/epub+zip"/></entry>
</feed>)";
constexpr char SIM_PAGE_2[] = R"(<feed xmlns="http://www.w3.org/2005/Atom">
<entry><title>Frankenstein</title><id>urn:sim:3</id><author><name>Mary Shelley</name></author>
<link href="/books/3.epub" rel="http://opds-spec.org/acquisition" type="application/epub+zip"/></entry>
<entry><title>Scanned Atlas</title><id>urn:sim:4</id>
<link href="/books/4.pdf" rel="http://opds-spec.org/acquisition" type="application/pdf"/></entry>
</feed>)";
#endif

}  // namespace

// Buffered line reader over an open file. Records are bounded by the OPDS
// parser's field limits, so a line that overflows the buffer can only come
// from a damaged file; it is skipped whole rather than returned truncated.
class OpdsLibrarySyncActivity::LineReader {
 public:
  LineReader(HalFile& file, char* buffer, const size_t capacity) : file(file), buf(buffer), cap(capacity) {}

  bool next(std::string_view& line) {
    while (true) {
      const void* newline = start < end ? memchr(buf + start, '\n', end - start) : nullptr;
      if (newline) {
        const size_t lineStart = start;
        const size_t lineEnd = static_cast<size_t>(static_cast<const char*>(newline) - buf);
        start = lineEnd + 1;
        if (discarding) {
          discarding = false;
          continue;
        }
        line = std::string_view(buf + lineStart, lineEnd - lineStart);
        return true;
      }
      if (eof) {
        const bool hasTail = start < end && !discarding;
        if (hasTail) line = std::string_view(buf + start, end - start);
        start = end;
        return hasTail;
      }
      if (start > 0) {
        memmove(buf, buf + start, end - start);
        end -= start;
        start = 0;
      }
      if (end == cap) {
        LOG_ERR(LOG_TAG, "Skipping over-long record");
        discarding = true;
        start = end = 0;
        continue;
      }
      const int got = file.read(buf + end, cap - end);
      if (got <= 0) {
        eof = true;
      } else {
        end += static_cast<size_t>(got);
      }
    }
  }

 private:
  HalFile& file;
  char* buf;
  size_t cap;
  size_t start = 0;
  size_t end = 0;
  bool eof = false;
  bool discarding = false;
};

OpdsLibrarySyncActivity::OpdsLibrarySyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                 OpdsServer server, std::string feedUrl, std::string feedTitle)
    : Activity("OpdsLibrarySync", renderer, mappedInput),
      server(std::move(server)),
      feedUrl(std::move(feedUrl)),
      feedTitle(std::move(feedTitle)),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

OpdsLibrarySyncActivity::~OpdsLibrarySyncActivity() = default;

void OpdsLibrarySyncActivity::onEnter() {
  Activity::onEnter();

  phase = Phase::SCANNING;
  stopReason = StopReason::NONE;
  started = false;
  cancelRequested = false;
  goHomeAfterCancel = false;
  // Credentials are only ever sent to the configured server's origin.
  authorizationOrigin = UrlUtils::ensureProtocol(server.url);
  char serverId[17];
  formatKey(opds_sync::fnv1a64(authorizationOrigin), serverId);
  ledgerPath = std::string("/.crosspoint/opds_sync_") + serverId + ".txt";

  uiReady = false;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_CONFIRM, &OpdsLibrarySyncActivity::onConfirmEvent, this);
  app.on(ACTION_CANCEL, &OpdsLibrarySyncActivity::onCancelEvent, this);
  app.setScreen(&OpdsLibrarySyncActivity::rootScreen, this);

  // One record buffer serves the ledger load and the download queue.
  recordBuffer = makeUniqueNoThrow<char[]>(RECORD_BUFFER_SIZE);
  if (!recordBuffer) {
    LOG_ERR(LOG_TAG, "OOM: record buffer");
    fail(tr(STR_MEMORY_ERROR));
    return;
  }
  requestUpdate();
}

void OpdsLibrarySyncActivity::onExit() {
  uiReady = false;
  pageFile.close();
  queueReader.reset();
  queueFile.close();
  libraryIndex.reset();
  removeWorkFiles();
  presentKeys.clear();
  metadataKeys.clear();
  recordBuffer.reset();
  if (downloaded > 0) library::invalidateLibraryIndex();
  Activity::onExit();
}

bool OpdsLibrarySyncActivity::preventAutoSleep() {
  return phase == Phase::SCANNING || phase == Phase::FETCHING || phase == Phase::DOWNLOADING;
}

// ---------------------------------------------------------------------------
// Input

void OpdsLibrarySyncActivity::onConfirmEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<OpdsLibrarySyncActivity*>(user);
  self->app.clearTapFlash();
  self->confirmPressed();
}

void OpdsLibrarySyncActivity::onCancelEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<OpdsLibrarySyncActivity*>(user);
  self->app.clearTapFlash();
  self->cancelPressed();
}

void OpdsLibrarySyncActivity::confirmPressed() {
  switch (phase) {
    case Phase::CONFIRM:
      queueFile = Storage.open(QUEUE_PATH, O_RDONLY);
      if (!queueFile) {
        LOG_ERR(LOG_TAG, "Cannot open queue");
        fail(tr(STR_OPDS_SYNC_STORAGE_ERROR));
        return;
      }
      queueReader = makeUniqueNoThrow<LineReader>(queueFile, recordBuffer.get(), RECORD_BUFFER_SIZE);
      if (!queueReader) {
        LOG_ERR(LOG_TAG, "OOM: queue reader");
        fail(tr(STR_MEMORY_ERROR));
        return;
      }
      {
        RenderLock lock;
        currentTitle.clear();
        currentProgress = currentTotal = 0;
        phase = Phase::DOWNLOADING;
      }
      requestUpdate();
      break;
    case Phase::DONE:
    case Phase::ERROR:
      finish();
      break;
    default:
      break;
  }
}

void OpdsLibrarySyncActivity::cancelPressed() {
  switch (phase) {
    case Phase::FETCHING:
    case Phase::DOWNLOADING:
      cancelRequested = true;
      break;
    case Phase::CONFIRM:
    case Phase::DONE:
    case Phase::ERROR:
      finish();
      break;
    default:
      break;
  }
}

// Polled from inside blocking network calls, the same way the OPDS browser
// lets Back, Home or the on-screen Cancel interrupt a download.
bool OpdsLibrarySyncActivity::pollCancel() {
  if (cancelRequested) return true;
  mappedInput.update();
  if (mappedInput.wasHomeGesture()) {
    goHomeAfterCancel = true;
    cancelRequested = true;
  }
  if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    cancelRequested = true;
  }
  if (uiReady) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) app.route(snap);
  }
  // app.route() can set cancelRequested through onCancelEvent() during this poll.
  // cppcheck-suppress knownConditionTrueFalse
  return cancelRequested;
}

void OpdsLibrarySyncActivity::loop() {
  if (uiReady) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) {
      app.route(snap);
      if (app.invalidated()) requestUpdate();
    }
  }
  if (phase == Phase::CONFIRM || phase == Phase::DONE || phase == Phase::ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      confirmPressed();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      cancelPressed();
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) cancelRequested = true;

  switch (phase) {
    case Phase::SCANNING:
      if (!started) {
        started = true;
        // Show the scanning screen before the card walk blocks this task.
        if (requestUpdateAndWait() != RequestUpdateResult::Rendered) requestUpdate(true);
        runScan();
      }
      break;
    case Phase::FETCHING:
      if (cancelRequested) {
        stopReason = StopReason::CANCELLED;
        finishWith(Phase::DONE);
      } else {
        fetchNextPage();
      }
      break;
    case Phase::DOWNLOADING:
      if (cancelRequested) {
        stopReason = StopReason::CANCELLED;
        finishWith(Phase::DONE);
      } else {
        downloadNext();
      }
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Local library

void OpdsLibrarySyncActivity::runScan() {
  presentKeys.clear();
  metadataKeys.clear();
  localBooks = 0;

  [[maybe_unused]] const unsigned long startMs = millis();
  auto path = makeUniqueNoThrow<char[]>(SCAN_PATH_SIZE);
  auto name = makeUniqueNoThrow<char[]>(SCAN_PATH_SIZE);
  if (!path || !name) {
    LOG_ERR(LOG_TAG, "OOM: scan buffers");
    fail(tr(STR_MEMORY_ERROR));
    return;
  }
  path[0] = '/';
  path[1] = '\0';
  if (!scanDirectory(path.get(), 1, name.get(), 0) || !loadLedger()) {
    fail(tr(STR_MEMORY_ERROR));
    return;
  }
  presentKeys.seal();
  addLibraryIndexKeys();
  LOG_INF(LOG_TAG, "Local scan: %u EPUBs, %u keys, %u metadata entries in %lu ms", static_cast<unsigned>(localBooks),
          static_cast<unsigned>(presentKeys.size()), static_cast<unsigned>(metadataKeys.size()), millis() - startMs);

  // A fresh queue for this run.
  Storage.remove(QUEUE_PATH);
  Storage.remove(PAGE_PATH);
  pageUrl.clear();
  nextPageUrl = feedUrl;
  pagesFetched = catalogueBooks = unsupportedEntries = missingBooks = 0;
  lastProgressRenderMs = 0;
  phase = Phase::FETCHING;
  requestUpdate();
}

bool OpdsLibrarySyncActivity::scanDirectory(char* path, const size_t pathLen, char* name, const int depth) {
  auto dir = Storage.open(path);
  if (!dir || !dir.isDirectory()) {
    LOG_ERR(LOG_TAG, "Cannot open folder: %s", path);
    return true;  // unreadable folders are skipped, never fatal
  }
  dir.rewindDirectory();
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(name, SCAN_PATH_SIZE);
    // Hidden entries include CrossInk's own .crosspoint cache.
    if (name[0] == '.' || strcmp(name, "System Volume Information") == 0) continue;
    if (file.isDirectory()) {
      if (depth >= MAX_SCAN_DEPTH) continue;
      const size_t nameLen = strlen(name);
      const size_t sep = (pathLen > 1) ? 1 : 0;  // root is "/"
      if (pathLen + sep + nameLen + 1 > SCAN_PATH_SIZE) continue;
      if (sep) path[pathLen] = '/';
      memcpy(path + pathLen + sep, name, nameLen + 1);
      file.close();
      const bool ok = scanDirectory(path, pathLen + sep + nameLen, name, depth + 1);
      path[pathLen] = '\0';
      if (!ok) return false;
      continue;
    }
    if (!opds_sync::isEpubFilename(name)) continue;
    ++localBooks;
    if (!presentKeys.add(opds_sync::localFilenameKey(name))) {
      LOG_ERR(LOG_TAG, "OOM: local key set at %u books", static_cast<unsigned>(localBooks));
      return false;
    }
  }
  dir.close();
  return true;
}

// Adds EPUB title/author keys from the Library index, when one exists, so
// books with unusual file names still count as present.
void OpdsLibrarySyncActivity::addLibraryIndexKeys() {
  libraryIndex = makeUniqueNoThrow<library::LibraryIndexFile>();
  if (!libraryIndex || !libraryIndex->open(library::libraryIndexPath())) {
    LOG_INF(LOG_TAG, "No usable Library index; matching by file name only");
    libraryIndex.reset();
    return;
  }
  std::string name;
  std::string title;
  std::string author;
  const uint16_t count = libraryIndex->bookCount();
  for (uint16_t ordinal = 0; ordinal < count; ++ordinal) {
    library::ClixRecord record{};
    if (!libraryIndex->readRecord(ordinal, record)) break;
    if (!libraryIndex->readName(record, name) || !opds_sync::isEpubFilename(name)) continue;
    if (!libraryIndex->readTitle(record, title) || !libraryIndex->readAuthor(record, author)) continue;
    const uint64_t key = opds_sync::metadataKey(title, author);
    if (key == 0) continue;
    if (!metadataKeys.add((key & ~ORDINAL_MASK) | ordinal)) {
      // Matching still works by file name; this layer is an extra.
      LOG_ERR(LOG_TAG, "OOM: metadata keys; continuing without them");
      metadataKeys.clear();
      libraryIndex.reset();
      return;
    }
  }
  metadataKeys.seal();
  if (metadataKeys.size() == 0) libraryIndex.reset();
}

// The ledger records books this device synced from this server, by their
// stable OPDS identity, so a later rename on the server does not cause a
// duplicate. Entries whose file has since gone are dropped, so deleting a
// book locally means the next sync fetches it again (as library-sync does).
bool OpdsLibrarySyncActivity::loadLedger() {
  if (!Storage.exists(ledgerPath.c_str())) return true;
  HalFile ledger = Storage.open(ledgerPath.c_str(), O_RDONLY);
  if (!ledger) return true;

  const std::string tmpPath = ledgerPath + LEDGER_TMP_SUFFIX;
  HalFile kept = Storage.open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
  size_t stale = 0;
  bool ok = true;
  LineReader reader(ledger, recordBuffer.get(), RECORD_BUFFER_SIZE);
  std::string_view line;
  std::string path;
  while (reader.next(line)) {
    std::string_view fields[2];
    uint64_t key = 0;
    if (splitFields(line, fields, 2) != 2 || !parseKey(fields[0], key) || fields[1].empty()) {
      ++stale;
      continue;
    }
    path.assign(fields[1].data(), fields[1].size());
    if (!Storage.exists(path.c_str())) {
      ++stale;
      continue;
    }
    if (!presentKeys.add(key)) {
      ok = false;
      break;
    }
    if (kept) {
      kept.write(line.data(), line.size());
      kept.write(static_cast<uint8_t>('\n'));
    }
  }
  ledger.close();
  const bool keptOk = kept && kept.sync();
  kept.close();
  if (ok && stale > 0 && keptOk) {
    Storage.remove(ledgerPath.c_str());
    Storage.rename(tmpPath.c_str(), ledgerPath.c_str());
    LOG_INF(LOG_TAG, "Ledger: dropped %u stale entries", static_cast<unsigned>(stale));
  } else {
    Storage.remove(tmpPath.c_str());
  }
  return ok;
}

void OpdsLibrarySyncActivity::appendLedger(const std::string& identity, const std::string& path) {
  HalFile ledger = Storage.open(ledgerPath.c_str(), O_WRONLY | O_CREAT | O_APPEND);
  if (!ledger) {
    LOG_ERR(LOG_TAG, "Cannot append ledger %s", ledgerPath.c_str());
    return;
  }
  std::string line = identity;
  line.push_back('\t');
  appendField(line, path);
  line.push_back('\n');
  ledger.write(line.data(), line.size());
  ledger.close();
}

// ---------------------------------------------------------------------------
// Catalogue walk

std::string OpdsLibrarySyncActivity::downloadPathFor(const std::string& title, const std::string& author) const {
  std::string path = SETTINGS.opdsDownloadFolder;
  path += '/';
  path += StringUtils::sanitizeFilename(
      opds_sync::bookFilenameBase(title, author, server.filenameFormat == OpdsFilenameFormat::TITLE_AUTHOR));
  path += ".epub";
  return path;
}

bool OpdsLibrarySyncActivity::metadataPresent(const uint64_t key) {
  if (key == 0 || !libraryIndex) return false;
  const uint64_t target = key & ~ORDINAL_MASK;
  std::string path;
  for (size_t i = metadataKeys.lowerBound(target); i < metadataKeys.size(); ++i) {
    const uint64_t entry = metadataKeys.at(i);
    if ((entry & ~ORDINAL_MASK) != target) break;
    library::ClixRecord record{};
    // The index can lag behind the card; only a file that is still there counts.
    if (libraryIndex->readRecord(static_cast<uint16_t>(entry & ORDINAL_MASK), record) &&
        libraryIndex->readPath(record, path) && Storage.exists(path.c_str())) {
      return true;
    }
  }
  return false;
}

bool OpdsLibrarySyncActivity::isPresent(const OpdsEntry& entry, const std::string& absoluteHref) {
  if (presentKeys.contains(opds_sync::remoteIdentityKey(entry.id, absoluteHref))) return true;

  opds_sync::CandidateKeys candidates;
  candidates.addBookNames(entry.title, entry.author, entry.href);
  // The exact names the downloader writes, which sanitising may have shortened.
  candidates.addStem(StringUtils::sanitizeFilename(opds_sync::bookFilenameBase(entry.title, entry.author, false)));
  candidates.addStem(StringUtils::sanitizeFilename(opds_sync::bookFilenameBase(entry.title, entry.author, true)));
  for (size_t i = 0; i < candidates.size(); ++i) {
    if (presentKeys.contains(candidates[i])) return true;
  }
  return metadataPresent(opds_sync::metadataKey(entry.title, entry.author));
}

bool OpdsLibrarySyncActivity::onCatalogueEntry(void* context, OpdsEntry& entry) {
  auto* self = static_cast<OpdsLibrarySyncActivity*>(context);
  if (entry.type != OpdsEntryType::BOOK) return true;  // sub-catalogues are not followed
  ++self->pageBooks;

  const std::string absoluteHref = UrlUtils::buildUrl(self->pageUrl, entry.href);
  if (self->isPresent(entry, absoluteHref)) return true;

  char identity[17];
  formatKey(opds_sync::remoteIdentityKey(entry.id, absoluteHref), identity);
  std::string record;
  record.reserve(absoluteHref.size() + entry.title.size() + entry.author.size() + 24);
  record.append(identity);
  record.push_back('\t');
  appendField(record, absoluteHref);
  record.push_back('\t');
  appendField(record, entry.title);
  record.push_back('\t');
  appendField(record, entry.author);
  record.push_back('\n');
  if (self->pageFile.write(record.data(), record.size()) != record.size()) {
    LOG_ERR(LOG_TAG, "Queue write failed");
    self->pageWriteFailed = true;
    return false;
  }
  ++self->pageMissing;
  return true;
}

void OpdsLibrarySyncActivity::fetchNextPage() {
  if (nextPageUrl.empty()) {
    finishCatalogue();
    return;
  }
  if (pagesFetched >= MAX_PAGES) {
    LOG_ERR(LOG_TAG, "Catalogue exceeds %u pages", static_cast<unsigned>(MAX_PAGES));
    fail(tr(STR_OPDS_SYNC_TOO_MANY_PAGES));
    return;
  }
  const uint64_t pageKey = opds_sync::fnv1a64(nextPageUrl);
  for (size_t i = 0; i < pagesFetched; ++i) {
    if (visitedPages[i] == pageKey) {
      LOG_ERR(LOG_TAG, "Pagination repeated %s", UrlUtils::forLog(nextPageUrl).c_str());
      fail(tr(STR_OPDS_SYNC_PAGINATION_LOOP));
      return;
    }
  }
  pageUrl = nextPageUrl;
  LOG_DBG(LOG_TAG, "Page %u: %s", static_cast<unsigned>(pagesFetched + 1), UrlUtils::forLog(pageUrl).c_str());

  std::string pageNext;
  size_t pageSeen = 0;
  bool pageOk = false;
  for (int attempt = 1; attempt <= PAGE_ATTEMPTS && !pageOk; ++attempt) {
    pageBooks = pageMissing = 0;
    pageWriteFailed = false;
    pageFile = Storage.open(PAGE_PATH, O_WRONLY | O_CREAT | O_TRUNC);
    if (!pageFile) {
      LOG_ERR(LOG_TAG, "Cannot open page file");
      fail(tr(STR_OPDS_SYNC_STORAGE_ERROR));
      return;
    }
    OpdsParser parser(&OpdsLibrarySyncActivity::onCatalogueEntry, this);

#ifdef SIMULATOR
    const char* simFeed = pagesFetched == 0 ? SIM_PAGE_1 : SIM_PAGE_2;
    parser.parse(simFeed, strlen(simFeed));
    const auto result = HttpDownloader::OK;
#else
    HttpDownloader::DownloadOptions options;
    options.transport = HttpDownloader::Transport::WOLFSSL;
    options.authorizationOrigin = authorizationOrigin;
    options.shouldCancel = [this] { return pollCancel(); };
    const auto result = HttpDownloader::streamUrl(
        pageUrl,
        [&parser](const uint8_t* data, const size_t len) {
          parser.write(data, len);
          return !parser.error();  // stop the transfer as soon as the page is unusable
        },
        nullptr, server.username, server.password, std::move(options));
    if (result == HttpDownloader::OK) parser.flush();
#endif

    const bool closed = pageFile.close();
    if (result == HttpDownloader::ABORTED && cancelRequested) {
      stopReason = StopReason::CANCELLED;
      if (goHomeAfterCancel) {
        onGoHome();
        return;
      }
      mappedInput.suppressNextBackRelease();
      finishWith(Phase::DONE);
      return;
    }
    if (pageWriteFailed || (result == HttpDownloader::OK && !closed)) {
      fail(tr(STR_OPDS_SYNC_STORAGE_ERROR));
      return;
    }
    if (result == HttpDownloader::OK && !parser.error()) {
      pageOk = true;
      pageNext = parser.getNextPageUrl();
      pageSeen = parser.getEntriesSeen();
    } else {
      LOG_ERR(LOG_TAG, "Page %u attempt %d failed (http %d, parse %d)", static_cast<unsigned>(pagesFetched + 1),
              attempt, static_cast<int>(result), static_cast<int>(parser.getErrorReason()));
    }
  }

  if (!pageOk) {
    // library-sync stops before downloading anything when the catalogue
    // cannot be read completely; partial results would look like a full sync.
    fail(tr(STR_FETCH_FEED_FAILED));
    return;
  }
  if (!appendPageToQueue()) {
    fail(tr(STR_OPDS_SYNC_STORAGE_ERROR));
    return;
  }

  visitedPages[pagesFetched++] = pageKey;
  catalogueBooks += pageBooks;
  missingBooks += pageMissing;
  unsupportedEntries += pageSeen > pageBooks ? pageSeen - pageBooks : 0;
  nextPageUrl = pageNext.empty() ? std::string() : UrlUtils::buildUrl(pageUrl, pageNext);

  if (nextPageUrl.empty()) {
    finishCatalogue();
    return;
  }
  const unsigned long now = millis();
  if (lastProgressRenderMs == 0 || now - lastProgressRenderMs >= CATALOGUE_PROGRESS_MIN_UPDATE_MS) {
    lastProgressRenderMs = now;
    requestUpdate();
  }
}

bool OpdsLibrarySyncActivity::appendPageToQueue() {
  if (pageMissing == 0) return true;
  HalFile page = Storage.open(PAGE_PATH, O_RDONLY);
  HalFile queue = Storage.open(QUEUE_PATH, O_WRONLY | O_CREAT | O_APPEND);
  if (!page || !queue) {
    LOG_ERR(LOG_TAG, "Cannot open queue files");
    return false;
  }
  char* buffer = recordBuffer.get();
  bool ok = true;
  while (true) {
    const int got = page.read(buffer, RECORD_BUFFER_SIZE);
    if (got < 0) {
      ok = false;
      break;
    }
    if (got == 0) break;
    if (queue.write(buffer, static_cast<size_t>(got)) != static_cast<size_t>(got)) {
      ok = false;
      break;
    }
  }
  page.close();
  if (!queue.close()) ok = false;
  if (!ok) LOG_ERR(LOG_TAG, "Queue append failed");
  return ok;
}

void OpdsLibrarySyncActivity::finishCatalogue() {
  Storage.remove(PAGE_PATH);
  // Metadata checks are done; release the index handle before downloading.
  libraryIndex.reset();
  metadataKeys.clear();
  LOG_INF(LOG_TAG, "Catalogue: %u pages, %u books, %u missing, %u without EPUB", static_cast<unsigned>(pagesFetched),
          static_cast<unsigned>(catalogueBooks), static_cast<unsigned>(missingBooks),
          static_cast<unsigned>(unsupportedEntries));
  phase = missingBooks > 0 ? Phase::CONFIRM : Phase::DONE;
  requestUpdate();
}

// ---------------------------------------------------------------------------
// Downloads

bool OpdsLibrarySyncActivity::readQueueRecord(std::string& identity, std::string& url, std::string& title,
                                              std::string& author) {
  std::string_view line;
  while (queueReader && queueReader->next(line)) {
    std::string_view fields[4];
    if (splitFields(line, fields, 4) < 3 || fields[0].size() != 16 || fields[1].empty()) {
      LOG_ERR(LOG_TAG, "Skipping malformed queue record");
      ++failed;
      continue;
    }
    identity.assign(fields[0].data(), fields[0].size());
    url.assign(fields[1].data(), fields[1].size());
    title.assign(fields[2].data(), fields[2].size());
    author.assign(fields[3].data(), fields[3].size());
    return true;
  }
  return false;
}

void OpdsLibrarySyncActivity::downloadNext() {
  std::string identity;
  std::string url;
  std::string title;
  std::string author;
  if (!readQueueRecord(identity, url, title, author)) {
    finishWith(Phase::DONE);
    return;
  }
  ++queueIndex;

  const std::string path = downloadPathFor(title, author);
  if (!DownloadFileSwap::recover(path)) {
    ++failed;
    return;
  }
  // Never replace a file: a book that reached this name some other way (or a
  // catalogue that lists the same book twice) is left as it is.
  if (Storage.exists(path.c_str())) {
    ++alreadyPresent;
    return;
  }
  const char* folder = SETTINGS.opdsDownloadFolder;
  if (folder[0] != '\0' && !Storage.exists(folder) && !Storage.mkdir(folder)) {
    LOG_ERR(LOG_TAG, "Could not create download folder %s", folder);
    fail(tr(STR_DOWNLOAD_FAILED));
    return;
  }

  {
    // render() reads the title on the render task.
    RenderLock lock;
    currentTitle = title;
    currentProgress = currentTotal = 0;
  }
  requestUpdate(true);

#ifdef SIMULATOR
  (void)validateEpub;
  ++downloaded;
  appendLedger(identity, path);
  return;
#else
  LOG_DBG(LOG_TAG, "Downloading %s -> %s", UrlUtils::forLog(url).c_str(), path.c_str());
  HttpDownloader::DownloadOptions options;
  options.shouldCancel = [this] { return pollCancel(); };
  options.bufferSize = DOWNLOAD_BUFFER_SIZE;
  options.transport = HttpDownloader::Transport::WOLFSSL;
  options.authorizationOrigin = authorizationOrigin;
  options.stageAsPart = true;
  options.checkFreeSpace = true;
  options.validate = &validateEpub;

  int lastRenderedPercent = -1;
  unsigned long lastUpdateMs = 0;
  bool cancelFlag = false;
  const auto result = HttpDownloader::downloadToFile(
      url, path,
      [this, &lastRenderedPercent, &lastUpdateMs](const size_t done, const size_t total) {
        currentProgress = done;
        currentTotal = total;
        const int percent = total > 0 ? static_cast<int>(static_cast<uint64_t>(done) * 100 / total) : 0;
        const unsigned long now = millis();
        if (percent >= 100 || lastRenderedPercent < 0 ||
            percent >= lastRenderedPercent + DOWNLOAD_PROGRESS_STEP_PERCENT ||
            now - lastUpdateMs >= DOWNLOAD_PROGRESS_MIN_UPDATE_MS) {
          lastRenderedPercent = percent;
          lastUpdateMs = now;
          requestUpdate(true);
        }
      },
      &cancelFlag, server.username, server.password, std::move(options));

  switch (result) {
    case HttpDownloader::OK:
      ++downloaded;
      clearBookCache(path);
      appendLedger(identity, path);
      break;
    case HttpDownloader::ABORTED:
      stopReason = StopReason::CANCELLED;
      if (goHomeAfterCancel) {
        onGoHome();
        return;
      }
      mappedInput.suppressNextBackRelease();
      finishWith(Phase::DONE);
      return;
    case HttpDownloader::INSUFFICIENT_SPACE:
      stopReason = StopReason::SD_FULL;
      finishWith(Phase::DONE);
      return;
    default:
      // Like library-sync, one bad book does not stop the rest.
      LOG_ERR(LOG_TAG, "Download failed (%d): %s", static_cast<int>(result), title.c_str());
      ++failed;
      break;
  }
  requestUpdate();
#endif
}

// ---------------------------------------------------------------------------
// State changes

void OpdsLibrarySyncActivity::finishWith(const Phase next) {
  pageFile.close();
  queueReader.reset();
  queueFile.close();
  libraryIndex.reset();
  removeWorkFiles();
  phase = next;
  requestUpdate();
}

void OpdsLibrarySyncActivity::fail(const char* message) {
  {
    RenderLock lock;
    errorMessage = message ? message : "";
  }
  finishWith(Phase::ERROR);
}

void OpdsLibrarySyncActivity::removeWorkFiles() {
  if (Storage.exists(PAGE_PATH)) Storage.remove(PAGE_PATH);
  if (Storage.exists(QUEUE_PATH)) Storage.remove(QUEUE_PATH);
}

// ---------------------------------------------------------------------------
// Rendering

void OpdsLibrarySyncActivity::rootScreen(UiApp::ScreenType& screen, void* user) {
  auto* self = static_cast<OpdsLibrarySyncActivity*>(user);
  self->buildHeader(screen);
  self->buildBody(screen);
}

void OpdsLibrarySyncActivity::buildHeader(UiApp::ScreenType& screen) {
  screen.takeBottom(static_cast<int16_t>(UITheme::getInstance().getMetrics().buttonHintsHeight));
  fui::HeaderProps header;
  header.title = tr(STR_OPDS_LIBRARY_SYNC);
  header.borderEdges = fui::EdgeBottom;
  screen.header(header);
  screen.spacer(static_cast<int16_t>(UITheme::getInstance().getMetrics().verticalSpacing));
}

void OpdsLibrarySyncActivity::addLine(UiApp::ScreenType& screen, const char* text, const bool title) {
  fui::TextStyle style = screen.theme().bodyText;
  style.align = fui::TextAlign::Center;
  style.bold = title;
  const int16_t lh = screen.target().lineHeight(style.font);
  screen.target().text(screen.takeTop(lh, screen.theme().spaceMd), text, style);
}

void OpdsLibrarySyncActivity::buildBody(UiApp::ScreenType& screen) {
  const auto& theme = screen.theme();
  const int16_t lh = screen.target().lineHeight(theme.bodyText.font);
  const int16_t gap = theme.spaceMd;
  const int16_t btnH = theme.rowHeight;
  char line[160];

  // Count lines first so the block can be centred vertically.
  int lines = 0;
  bool showBar = false;
  int buttons = 0;
  switch (phase) {
    case Phase::SCANNING:
      lines = 2;
      break;
    case Phase::FETCHING:
      lines = 3;
      buttons = 1;
      break;
    case Phase::CONFIRM:
      lines = unsupportedEntries > 0 ? 4 : 3;
      buttons = 2;
      break;
    case Phase::DOWNLOADING:
      lines = 3;
      showBar = true;
      buttons = 1;
      break;
    case Phase::DONE:
      lines = 5;
      buttons = 1;
      break;
    case Phase::ERROR:
      lines = 2;
      buttons = 1;
      break;
  }
  const int16_t blockH = static_cast<int16_t>(lines * (lh + gap) + (showBar ? 16 + gap : 0) + (buttons > 0 ? btnH : 0));
  const fui::Rect body = screen.body();
  if (body.height > blockH) screen.spacer(static_cast<int16_t>((body.height - blockH) / 2));

  const char* folder = SETTINGS.opdsDownloadFolder[0] != '\0' ? SETTINGS.opdsDownloadFolder : "/";
  switch (phase) {
    case Phase::SCANNING:
      addLine(screen, tr(STR_OPDS_SYNC_SCANNING_DEVICE), true);
      addLine(screen, feedTitle.c_str());
      break;
    case Phase::FETCHING:
      addLine(screen, tr(STR_OPDS_SYNC_CHECKING_CATALOGUE), true);
      addLine(screen, feedTitle.c_str());
      snprintf(line, sizeof(line), tr(STR_OPDS_SYNC_CATALOGUE_PROGRESS), static_cast<unsigned>(pagesFetched),
               static_cast<unsigned>(catalogueBooks), static_cast<unsigned>(missingBooks));
      addLine(screen, line);
      break;
    case Phase::CONFIRM:
      snprintf(line, sizeof(line), tr(STR_OPDS_SYNC_MISSING_SUMMARY), static_cast<unsigned>(missingBooks),
               static_cast<unsigned>(catalogueBooks));
      addLine(screen, line, true);
      addLine(screen, feedTitle.c_str());
      snprintf(line, sizeof(line), tr(STR_OPDS_SYNC_DOWNLOAD_TO), folder);
      addLine(screen, line);
      if (unsupportedEntries > 0) {
        snprintf(line, sizeof(line), tr(STR_OPDS_SYNC_UNSUPPORTED), static_cast<unsigned>(unsupportedEntries));
        addLine(screen, line);
      }
      break;
    case Phase::DOWNLOADING: {
      snprintf(line, sizeof(line), tr(STR_OPDS_SYNC_DOWNLOADING_PROGRESS), static_cast<unsigned>(queueIndex),
               static_cast<unsigned>(missingBooks));
      addLine(screen, line, true);
      addLine(screen, currentTitle.c_str());
      const fui::Rect bar = screen.takeTop(16, gap).inset(fui::Insets{0, 50, 0, 50});
      if (currentTotal > 0) {
        fui::ProgressBarProps progress;
        progress.value = static_cast<int32_t>(currentProgress);
        progress.max = static_cast<int32_t>(currentTotal);
        progress.border = fui::Paint::solid(fui::Color::Black);
        progress.borderWidth = 1;
        fui::progressBar(screen.frame(), bar, progress);
      }
      snprintf(line, sizeof(line), tr(STR_OPDS_SYNC_DOWNLOAD_COUNTS), static_cast<unsigned>(downloaded),
               static_cast<unsigned>(failed));
      addLine(screen, line);
      break;
    }
    case Phase::DONE:
      if (stopReason == StopReason::CANCELLED) {
        addLine(screen, tr(STR_OPDS_SYNC_CANCELLED), true);
      } else if (stopReason == StopReason::SD_FULL) {
        addLine(screen, tr(STR_SD_CARD_FULL), true);
      } else if (missingBooks == 0) {
        addLine(screen, tr(STR_OPDS_SYNC_UP_TO_DATE), true);
      } else {
        addLine(screen, tr(STR_OPDS_SYNC_COMPLETE), true);
      }
      addLine(screen, feedTitle.c_str());
      snprintf(line, sizeof(line), tr(STR_OPDS_SYNC_RESULT_DOWNLOADED), static_cast<unsigned>(downloaded));
      addLine(screen, line);
      // Books matched during the catalogue walk, plus queued ones that turned
      // out to exist by the time their turn came.
      snprintf(line, sizeof(line), tr(STR_OPDS_SYNC_RESULT_PRESENT),
               static_cast<unsigned>(catalogueBooks - missingBooks + alreadyPresent));
      addLine(screen, line);
      snprintf(line, sizeof(line), tr(STR_OPDS_SYNC_RESULT_FAILED), static_cast<unsigned>(failed));
      addLine(screen, line);
      break;
    case Phase::ERROR:
      addLine(screen, tr(STR_ERROR_MSG), true);
      addLine(screen, errorMessage.c_str());
      break;
  }

  if (buttons == 0) return;
  const fui::Rect btnArea = screen.takeTop(btnH);
  if (buttons == 1) {
    const int16_t btnW = static_cast<int16_t>(btnArea.width / 3);
    fui::ButtonProps button;
    const bool cancels = phase == Phase::FETCHING || phase == Phase::DOWNLOADING;
    button.label = cancels ? tr(STR_CANCEL) : tr(STR_OK);
    button.action = cancels ? ACTION_CANCEL : ACTION_CONFIRM;
    screen.button(button,
                  fui::Rect{static_cast<int16_t>(btnArea.x + (btnArea.width - btnW) / 2), btnArea.y, btnW, btnH});
    return;
  }
  const int16_t btnW = static_cast<int16_t>(btnArea.width / 3);
  const int16_t spacing = static_cast<int16_t>(btnArea.width / 12);
  const int16_t left = static_cast<int16_t>(btnArea.x + (btnArea.width - btnW * 2 - spacing) / 2);
  fui::ButtonProps cancel;
  cancel.label = tr(STR_CANCEL);
  cancel.action = ACTION_CANCEL;
  screen.button(cancel, fui::Rect{left, btnArea.y, btnW, btnH});
  fui::ButtonProps download;
  download.label = tr(STR_DOWNLOAD);
  download.action = ACTION_CONFIRM;
  screen.button(download, fui::Rect{static_cast<int16_t>(left + btnW + spacing), btnArea.y, btnW, btnH});
}

void OpdsLibrarySyncActivity::render(RenderLock&&) {
  renderer.clearScreen();

  MappedInputManager::Labels labels;
  switch (phase) {
    case Phase::FETCHING:
    case Phase::DOWNLOADING:
      labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
      break;
    case Phase::CONFIRM:
      labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_CANCEL)), tr(STR_DOWNLOAD), "", "");
      break;
    case Phase::DONE:
    case Phase::ERROR:
      labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_OK), "", "");
      break;
    default:
      labels = mappedInput.mapLabels("", "", "", "");
      break;
  }
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  uiReady = false;
  app.render();
  uiReady = true;
  renderer.displayBuffer(screenTransitionRefresh.modeFor(static_cast<uint8_t>(phase)));
}
