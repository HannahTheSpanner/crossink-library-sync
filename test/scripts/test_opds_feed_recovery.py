#!/usr/bin/env python3
"""Run production OPDS fetch/cleanup with real Expat and scripted HTTP failures.

The screen and transport are doubles; this does not model ESP32 Wi-Fi memory.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "src/activities/browser/OpdsBookBrowserActivity.cpp").read_text()


def function(signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2]


prefix = r'''
#include <OpdsParser.h>
#include <OpdsFeedParser.h>
#include <OpdsSearchTemplate.h>
#include "util/UrlUtils.h"
#include <vector>
#include <Memory.h>
#include <XmlParserUtils.h>
#include "util/OpdsFeedError.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#define LOG_DBG(...) ((void)0)
#define LOG_ERR(...) ((void)0)
inline void delay(unsigned long) {}
constexpr int FEED_ATTEMPTS = 3;
constexpr unsigned long RETRY_BACKOFF_MS = 1000;
// The real retryBackoff() polls the cancel check during a timed pause; the
// harness has no clock, so it checks once and does not wait.
template <typename Cancelled>
bool retryBackoff(unsigned long, Cancelled&& cancelled) { return cancelled(); }
struct MappedInputManager { enum class Button { Back }; };
struct RenderLock {};
struct I18n { static I18n& getInstance() { static I18n value; return value; } const char* getCode() const { return "en"; } };
void replaceFolderEmoji(std::string&) {}
constexpr const char *STR_MEMORY_ERROR = "memory", *STR_NO_SERVER_URL = "no server",
  *STR_OPDS_FEED_BUFFER_MEMORY_ERROR = "buffer memory", *STR_PARSE_FEED_FAILED = "parse",
  *STR_FEED_UNAUTHORIZED = "unauthorized", *STR_FEED_REDIRECTED = "redirected",
  *STR_FEED_SERVER_ERROR = "server error", *STR_FEED_UNREACHABLE = "unreachable",
  *STR_FETCH_FEED_FAILED = "fetch", *STR_PREV_PAGE = "previous", *STR_NEXT_PAGE = "next",
  *STR_NO_ENTRIES = "empty", *STR_OPDS_SYNC_MISSING = "Sync missing books";
const char* tr(const char* text) { return text; }
struct HttpDownloader {
  enum DownloadError { OK, HTTP_ERROR, UNAUTHORIZED, FILE_ERROR };
  enum class Transport { WOLFSSL };
  struct DownloadOptions { Transport transport; std::string_view authorizationOrigin; };
  static inline std::string response, requestedUrl;
  static inline DownloadError outcome = OK;
  static inline int status = 200;
  static inline bool withheld = false;
  static inline size_t delivered = 0;
  static inline int requests = 0;
  static inline int failFirst = 0;  // leading attempts answered with 503
  static bool isRetryable(DownloadError result, int responseStatus) {
    if (result != HTTP_ERROR) return false;
    return responseStatus <= 0 || responseStatus == 200 || responseStatus == 206 || responseStatus == 408 ||
           responseStatus == 429 || responseStatus >= 500;
  }
  static DownloadError streamUrl(const std::string& url, const std::function<bool(const uint8_t*, size_t)>& cb,
                                std::nullptr_t, const std::string&, const std::string&, DownloadOptions options,
                                HttpRequestDiagnostics* diagnostics = nullptr) {
    requestedUrl = url;
    ++requests;
    assert(options.authorizationOrigin == "http://opds.example/recent");
    HttpRequestDiagnostics local;
    if (!diagnostics) diagnostics = &local;
    if (failFirst > 0) {
      --failFirst;
      *diagnostics = {503, true, false, false};
      return HTTP_ERROR;
    }
    *diagnostics = {status, true, withheld, false};
    delivered = 0;
    for (size_t offset = 0; offset < response.size(); offset += 37) {
      const size_t n = std::min(size_t{37}, response.size() - offset);
      delivered += n;
      if (!cb(reinterpret_cast<const uint8_t*>(response.data() + offset), n)) {
        diagnostics->callbackRejected = true;
        return FILE_ERROR;
      }
    }
    return outcome;
  }
};
struct OpdsBookBrowserActivity {
  enum class BrowserState { ERROR, BROWSING };
  static constexpr size_t OPDS_BROWSER_ENTRY_CAPACITY = MAX_OPDS_FEED_ENTRIES + 3;
  std::unique_ptr<OpdsEntry[]> entries;
  size_t entryCount = 0;
  bool uiReady = true, hasPrevPageRow = false, hasNextPageRow = false, hasSyncRow = false;
  int selectorIndex = 0, topIndex = 0;
  BrowserState state = BrowserState::ERROR;
  std::string errorMessage, searchTemplate, currentPath, feedRootPath, feedTitle, syncFeedUrl, syncFeedTitle;
  struct HistoryEntry { std::string path, feedRoot, feedTitle; };
  std::vector<HistoryEntry> navigationHistory;
  std::unique_ptr<int> retryDownload;
  void showLoadingBeforeFetch() {}
  void performSearch(const std::string&);
  struct { std::string url = "http://opds.example/recent", username, password; } server;
  struct Input {
    std::string withPreviousPageArrow(const char* text) { return text; }
    std::string withNextPageArrow(const char* text) { return text; }
    std::string resolveLabel(const std::string& text) { return text; }
    void update() {}
    bool wasPressed(MappedInputManager::Button) const { return false; }
    bool wasReleased(MappedInputManager::Button) const { return false; }
  } mappedInput;
  void requestUpdate() {}
  bool ensureEntryBuffer();
  void clearEntries();
  bool appendEntry(OpdsEntry&&);
  void fetchFeed(const std::string&);
  bool loadPublication(OpdsEntry&, const std::string&);
  void addSyncRow(const std::string&, bool);
};
'''
main = r'''
void assertEmpty(const OpdsBookBrowserActivity& browser) {
  assert(browser.entryCount == 0);
  assert(!browser.uiReady && !browser.hasNextPageRow && !browser.hasPrevPageRow && !browser.hasSyncRow);
  for (size_t i = 0; i < browser.OPDS_BROWSER_ENTRY_CAPACITY; ++i) {
    const auto& entry = browser.entries[i];
    assert(entry.title.empty() && entry.author.empty() && entry.href.empty() && entry.id.empty());
    assert(entry.description[0] == '\0');
  }
}
int main() {
  const std::string book = "<entry><title>A recent book with a short title</title>"
    "<author><name>Example Author</name></author>"
    "<link href='/book.epub' rel='http://opds-spec.org/acquisition' type='application/epub+zip'/>"
    "<summary>" + std::string(1700, 'd') + "</summary></entry>";
  OpdsBookBrowserActivity browser;
  assert(browser.ensureEntryBuffer());
  // Reproduce partial entries with an unpublished browser count, including
  // the two pagination slots. Clearing must release every populated slot.
  browser.entries[browser.OPDS_BROWSER_ENTRY_CAPACITY - 1].title = std::string(200, 'p');
  browser.entries[10].href = std::string(200, 'h');
  browser.clearEntries();
  assertEmpty(browser);

  std::string full = "<feed>";
  for (size_t i = 0; i < MAX_OPDS_FEED_ENTRIES; ++i) full += book;
  full += "<link rel='previous' href='/recent?page=0'/><link rel='next' href='/recent?page=2'/></feed>";
  for (int attempt = 0; attempt < 3; ++attempt) {
    HttpDownloader::response = "<feed>" + book + book;
    HttpDownloader::outcome = HttpDownloader::HTTP_ERROR;
    browser.fetchFeed("");
    assert(browser.state == OpdsBookBrowserActivity::BrowserState::ERROR);
    assert(browser.errorMessage == STR_FETCH_FEED_FAILED);
    assertEmpty(browser);

    // A well-formed body with a failed HTTP transfer must also be discarded.
    HttpDownloader::response = full;
    browser.fetchFeed("");
    assertEmpty(browser);

    HttpDownloader::outcome = HttpDownloader::OK;
    browser.fetchFeed("");
    assert(browser.state == OpdsBookBrowserActivity::BrowserState::BROWSING);
    assert(browser.entryCount == browser.OPDS_BROWSER_ENTRY_CAPACITY);
    assert(browser.hasNextPageRow && browser.hasPrevPageRow && browser.hasSyncRow);
    // Row 0 syncs the catalogue the page belongs to; then previous page, books, next page.
    assert(browser.entries[0].title == STR_OPDS_SYNC_MISSING);
    assert(browser.syncFeedUrl == "http://opds.example/recent");
    assert(browser.entries[2].title == "A recent book with a short title");
    assert(browser.entries[2].description[0] == 'd');
    assert(browser.entries[1].href == "/recent?page=0");
    assert(browser.entries[browser.entryCount - 1].href == "/recent?page=2");
  }
  // Parser errors abort the transport and retain their own error message.
  HttpDownloader::response = "<feed>" + book + "</invalid>" + std::string(10000, 'x');
  browser.fetchFeed("");
  assert(browser.errorMessage == STR_PARSE_FEED_FAILED);
  assert(HttpDownloader::delivered < HttpDownloader::response.size());
  assertEmpty(browser);

  HttpDownloader::response = full;
  XmlTestHeap::limit = 0;
  browser.fetchFeed("");
  XmlTestHeap::limit = SIZE_MAX;
  assert(browser.errorMessage == STR_MEMORY_ERROR);
  assertEmpty(browser);
  // Force a buffer failure after construction via one oversized XML token.
  HttpDownloader::response = "<feed><entry attr='" + std::string(40000, 'x') + "'/></feed>";
  XmlTestHeap::limit = 16 * 1024;
  browser.fetchFeed("");
  XmlTestHeap::limit = SIZE_MAX;
  assert(browser.errorMessage == STR_OPDS_FEED_BUFFER_MEMORY_ERROR);
  assert(HttpDownloader::delivered < HttpDownloader::response.size());
  assertEmpty(browser);
  assert(XmlTestHeap::live == 0);

  for (const auto status : {401, 503, 0}) {
    HttpDownloader::status = status;
    HttpDownloader::outcome = status == 401 ? HttpDownloader::UNAUTHORIZED : HttpDownloader::HTTP_ERROR;
    HttpDownloader::response.clear();
    HttpDownloader::requests = 0;
    browser.fetchFeed("");
    assert(browser.errorMessage == (status == 401 ? STR_FEED_UNAUTHORIZED :
                                  status == 503 ? STR_FEED_SERVER_ERROR : STR_FEED_UNREACHABLE));
    // A rejected login is definitive; a server fault or no response is retried.
    assert(HttpDownloader::requests == (status == 401 ? 1 : FEED_ATTEMPTS));
    assertEmpty(browser);
  }
  // A transient failure followed by a good response shows the page.
  HttpDownloader::status = 200;
  HttpDownloader::outcome = HttpDownloader::OK;
  HttpDownloader::response = "<feed>" + book + "</feed>";
  HttpDownloader::requests = 0;
  HttpDownloader::failFirst = 1;
  browser.fetchFeed("");
  assert(HttpDownloader::requests == 2);
  assert(browser.state == OpdsBookBrowserActivity::BrowserState::BROWSING);
  assert(browser.entryCount == 2 && browser.hasSyncRow);
  HttpDownloader::status = 200;
  HttpDownloader::outcome = HttpDownloader::OK;
  HttpDownloader::response = full;
  browser.fetchFeed("");
  assert(browser.entryCount == browser.OPDS_BROWSER_ENTRY_CAPACITY);
  assert(XmlTestHeap::live == 0);
  HttpDownloader::response = R"({"navigation":[{"title":"JSON folder","href":"next"}],"links":[{"rel":"next","href":"page2"}]})";
  browser.fetchFeed("");
  assert(browser.entryCount == 2 && browser.hasNextPageRow);
  assert(browser.entries[0].title == "JSON folder");
  HttpDownloader::response = R"({"navigation":[{"title":"Partial JSON folder","href":"next"}],)";
  browser.fetchFeed("");
  assert(browser.errorMessage == STR_PARSE_FEED_FAILED);
  assertEmpty(browser);
  HttpDownloader::response = R"({"navigation":[{"title":"JSON retry","href":"next"}]})";
  browser.fetchFeed("");
  assert(browser.entryCount == 1 && browser.entries[0].title == "JSON retry");
  assert(!browser.hasSyncRow);
  // A navigation-only root offers to sync its "All Books" catalogue.
  browser.currentPath = "/opds/root.xml";
  HttpDownloader::response =
      "<feed><entry><title>Authors</title><link href='authors' type='application/atom+xml'/></entry>"
      "<entry><title>All Books</title><link href='all?page=1' type='application/atom+xml'/></entry></feed>";
  browser.fetchFeed(browser.currentPath);
  assert(browser.hasSyncRow && browser.entryCount == 3);
  assert(browser.syncFeedTitle == "All Books");
  assert(browser.syncFeedUrl == "http://opds.example/opds/all?page=1");
  // OPDS 2 group previews are not a catalogue; the "All books" link is.
  HttpDownloader::response =
      R"({"navigation":[{"title":"All books","href":"/catalog/all.json"}],"groups":[{"metadata":{"title":"New"},)"
      R"("links":[{"rel":"self","href":"/new.json"}],"publications":[{"metadata":{"title":"Preview"},)"
      R"("links":[{"rel":"acquisition","type":"application/epub+zip","href":"/p.epub"}]}]}]})";
  browser.fetchFeed(browser.currentPath);
  assert(browser.hasSyncRow && browser.syncFeedUrl == "http://opds.example/catalog/all.json");
  browser.currentPath.clear();
  OpdsEntry selected;
  selected.title = "Feed title";
  selected.href = "old.epub";
  selected.selfHref = "details.json";
  HttpDownloader::response = R"({"metadata":{"title":"Publication title","description":"Details text"},"links":[{"rel":"download","type":"application/epub+zip","href":"new.epub"}]})";
  assert(browser.loadPublication(selected, "http://opds.example/details.json"));
  assert(selected.title == "Publication title" && selected.href == "http://opds.example/new.epub" && selected.selfHref.empty());
  assert(std::string(selected.description.data()) == "Details text");
  HttpDownloader::response = "{\"metadata\":{";
  assert(!browser.loadPublication(selected, "http://opds.example/details.json"));
  assert(selected.title == "Publication title" && selected.href == "http://opds.example/new.epub");
  HttpDownloader::outcome = HttpDownloader::OK;
  HttpDownloader::status = 200;
  HttpDownloader::response = R"({"navigation":[{"title":"Result","href":"result"}]})";
  browser.currentPath = "/catalog/fiction/index.json?page=1";
  browser.searchTemplate = "search{?query,title,author}";
  browser.performSearch("a & café");
  assert(HttpDownloader::requestedUrl == "http://opds.example/catalog/fiction/search?query=a%20%26%20caf%C3%A9");
  assert(browser.navigationHistory.back().path == "/catalog/fiction/index.json?page=1");
  browser.currentPath = "/catalog/fiction/index.json?page=1";
  browser.searchTemplate = "{?query}";
  browser.performSearch("book");
  assert(HttpDownloader::requestedUrl == "http://opds.example/catalog/fiction/index.json?query=book");
  browser.searchTemplate = "../search?q={searchTerms}&page={startPage?}";
  browser.performSearch("new book");
  assert(HttpDownloader::requestedUrl == "http://opds.example/catalog/search?q=new%20book&page=");
  std::puts("OPDS Atom/JSON cleanup, retry, pagination, sync row, parser OOM, publication fetch and HTTP errors passed");
}
'''

with tempfile.TemporaryDirectory(prefix="crossink-opds-recovery-") as temp:
    temp = Path(temp)
    harness = temp / "recovery.cpp"
    harness.write_text(prefix + "\n".join(function(signature) for signature in (
        "bool isAllBooksEntry(",
        "void OpdsBookBrowserActivity::addSyncRow(",
        "void OpdsBookBrowserActivity::fetchFeed(",
        "void OpdsBookBrowserActivity::performSearch(",
        "bool OpdsBookBrowserActivity::loadPublication(",
        "bool OpdsBookBrowserActivity::ensureEntryBuffer(",
        "void OpdsBookBrowserActivity::clearEntries(",
        "bool OpdsBookBrowserActivity::appendEntry(",
    )) + main)
    flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
             "-DXML_CONTEXT_BYTES=0", "-DXML_GE=0"]
    includes = ["-I" + str(root / p) for p in (
        "test/opds_parser/stubs", "lib/OpdsParser", "lib/XmlParserUtils", "lib/expat", "lib/Memory", "freeink-sdk/libs/network/JsonSax/include", "src")]
    objects = []
    for name in ("xmlparse", "xmlrole", "xmltok"):
        obj = temp / (name + ".o")
        subprocess.run([os.environ.get("CC", "cc"), "-std=c99", *flags, *includes,
                        "-c", str(root / "lib/expat" / (name + ".c")), "-o", str(obj)], check=True)
        objects.append(str(obj))
    binary = temp / "recovery"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", *flags, *includes,
                    str(harness), str(root / "lib/OpdsParser/OpdsParser.cpp"),
                    str(root / "lib/OpdsParser/OpdsFeedParser.cpp"),
                    str(root / "lib/OpdsParser/OpdsJsonParser.cpp"),
                    str(root / "lib/OpdsParser/OpdsSearchTemplate.cpp"), str(root / "src/util/UrlUtils.cpp"),
                    str(root / "freeink-sdk/libs/network/JsonSax/src/StreamingJsonParser.cpp"), *objects, "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
