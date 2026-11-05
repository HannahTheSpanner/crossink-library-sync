#include "OpdsSyncQueue.h"

#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace {
constexpr char LOG_TAG[] = "OSQ";
constexpr char LEDGER_TMP_SUFFIX[] = ".tmp";
// Queue record link kinds: a direct EPUB, or an OPDS 2 publication document
// to resolve to one before downloading.
constexpr char LINK_EPUB = 'e';
constexpr char LINK_PUBLICATION = 'p';

// Tabs and newlines delimit records; nothing else is reserved.
void appendField(std::string& out, const std::string& value) {
  std::transform(value.begin(), value.end(), std::back_inserter(out),
                 [](const char c) { return (c == '\t' || c == '\n' || c == '\r') ? ' ' : c; });
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
}  // namespace

// Buffered line reader over an open file. Records are bounded by the OPDS
// parser's field limits, so a line that overflows the buffer can only come
// from a damaged file; it is skipped whole rather than returned truncated.
class OpdsSyncQueue::LineReader {
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

OpdsSyncQueue::OpdsSyncQueue(const char* queuePath, const char* pagePath, std::string ledgerPath, char* buffer)
    : queuePath(queuePath), pagePath(pagePath), ledgerPath(std::move(ledgerPath)), buffer(buffer) {}

OpdsSyncQueue::~OpdsSyncQueue() {
  pageFile.close();
  closeReader();
}

void OpdsSyncQueue::formatKey(const uint64_t key, char (&out)[KEY_TEXT_SIZE]) {
  snprintf(out, sizeof(out), "%08lx%08lx", static_cast<unsigned long>(key >> 32),
           static_cast<unsigned long>(key & 0xFFFFFFFFULL));
}

bool OpdsSyncQueue::parseKey(const std::string_view text, uint64_t& out) {
  if (text.size() != KEY_TEXT_SIZE - 1) return false;
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

// ---------------------------------------------------------------------------
// Ledger

bool OpdsSyncQueue::loadLedger(opds_sync::KeySet& present, size_t& stale) {
  stale = 0;
  if (!Storage.exists(ledgerPath.c_str())) return true;
  HalFile ledger = Storage.open(ledgerPath.c_str(), O_RDONLY);
  if (!ledger) return true;

  const std::string tmpPath = ledgerPath + LEDGER_TMP_SUFFIX;
  HalFile kept = Storage.open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
  bool ok = true;
  LineReader lines(ledger, buffer, RECORD_BUFFER_SIZE);
  std::string_view line;
  std::string path;
  while (lines.next(line)) {
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
    if (!present.add(key)) {
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

void OpdsSyncQueue::appendLedger(const std::string& identity, const std::string& path) {
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

void OpdsSyncQueue::reset() {
  pageFile.close();
  closeReader();
  removeWorkFiles();
  skipped = 0;
}

bool OpdsSyncQueue::beginPage() {
  pageFile.close();
  pageFile = Storage.open(pagePath, O_WRONLY | O_CREAT | O_TRUNC);
  if (!pageFile) LOG_ERR(LOG_TAG, "Cannot open page file");
  return static_cast<bool>(pageFile);
}

bool OpdsSyncQueue::appendPageRecord(const uint64_t identity, const bool publicationDocument, const std::string& url,
                                     const std::string& title, const std::string& author) {
  char key[KEY_TEXT_SIZE];
  formatKey(identity, key);
  std::string record;
  record.reserve(url.size() + title.size() + author.size() + 24);
  record.append(key);
  record.push_back('\t');
  record.push_back(publicationDocument ? LINK_PUBLICATION : LINK_EPUB);
  record.push_back('\t');
  appendField(record, url);
  record.push_back('\t');
  appendField(record, title);
  record.push_back('\t');
  appendField(record, author);
  record.push_back('\n');
  if (pageFile.write(record.data(), record.size()) != record.size()) {
    LOG_ERR(LOG_TAG, "Queue write failed");
    return false;
  }
  return true;
}

bool OpdsSyncQueue::endPage() { return pageFile.close(); }

bool OpdsSyncQueue::commitPage(const size_t records) {
  if (records == 0) return true;
  HalFile page = Storage.open(pagePath, O_RDONLY);
  HalFile queue = Storage.open(queuePath, O_WRONLY | O_CREAT | O_APPEND);
  if (!page || !queue) {
    LOG_ERR(LOG_TAG, "Cannot open queue files");
    return false;
  }
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

// ---------------------------------------------------------------------------
// Downloads

bool OpdsSyncQueue::openForReading() {
  closeReader();
  queueFile = Storage.open(queuePath, O_RDONLY);
  if (!queueFile) {
    LOG_ERR(LOG_TAG, "Cannot open queue");
    return false;
  }
  reader = makeUniqueNoThrow<LineReader>(queueFile, buffer, RECORD_BUFFER_SIZE);
  if (!reader) {
    LOG_ERR(LOG_TAG, "OOM: queue reader");
    queueFile.close();
    return false;
  }
  return true;
}

bool OpdsSyncQueue::next(Record& record) {
  std::string_view line;
  while (reader && reader->next(line)) {
    std::string_view fields[5];
    if (splitFields(line, fields, 5) < 4 || fields[0].size() != KEY_TEXT_SIZE - 1 || fields[1].size() != 1 ||
        fields[2].empty()) {
      LOG_ERR(LOG_TAG, "Skipping malformed queue record");
      ++skipped;
      continue;
    }
    record.identity.assign(fields[0].data(), fields[0].size());
    record.publicationDocument = fields[1][0] == LINK_PUBLICATION;
    record.url.assign(fields[2].data(), fields[2].size());
    record.title.assign(fields[3].data(), fields[3].size());
    record.author.assign(fields[4].data(), fields[4].size());
    return true;
  }
  return false;
}

void OpdsSyncQueue::closeReader() {
  reader.reset();
  queueFile.close();
}

void OpdsSyncQueue::removeWorkFiles() {
  if (Storage.exists(pagePath)) Storage.remove(pagePath);
  if (Storage.exists(queuePath)) Storage.remove(queuePath);
}
