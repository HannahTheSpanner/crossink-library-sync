#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>
#include <HalStorage.h>
#include <OpdsLibraryMatch.h>
#include <OpdsParser.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "OpdsServerStore.h"
#include "activities/Activity.h"
#include "activities/ScreenTransitionRefresh.h"

namespace library {
class LibraryIndexFile;
}

/**
 * Bulk "sync missing books" for an OPDS catalogue, ported from the KOReader
 * library-sync plugin.
 *
 * The flow is the plugin's: index what is already on the card, walk every
 * page of the chosen acquisition feed, queue each EPUB that has no local
 * match, confirm the count with the user, then download the queue one book at
 * a time. Nothing is deleted or replaced: books already on the card (by file
 * name, EPUB metadata, or an earlier sync of the same server entry) are left
 * alone.
 *
 * Memory stays flat regardless of catalogue size. Feed pages are parsed in
 * streaming mode, the missing-book queue lives on the SD card, and the only
 * structure that scales with the library is a sorted array of 8-byte keys for
 * the local books.
 */
class OpdsLibrarySyncActivity final : public Activity {
 public:
  OpdsLibrarySyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, OpdsServer server,
                          std::string feedUrl, std::string feedTitle);
  ~OpdsLibrarySyncActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;

 private:
  enum class Phase : uint8_t { SCANNING, FETCHING, CONFIRM, DOWNLOADING, DONE, ERROR };
  enum class StopReason : uint8_t { NONE, CANCELLED, SD_FULL };

  using UiApp = freeink::ui::FreeInkApp<8, 4>;
  class LineReader;

  // Safety limits carried over from library-sync. A feed that loops or never
  // ends must not keep the radio up forever.
  static constexpr size_t MAX_PAGES = 200;
  static constexpr int PAGE_ATTEMPTS = 3;
  // One queue record: identity, URL (parser caps at 768), title (160) and
  // author (120) plus separators, with headroom.
  static constexpr size_t RECORD_BUFFER_SIZE = 1280;
  static constexpr int MAX_SCAN_DEPTH = 6;
  static constexpr size_t SCAN_PATH_SIZE = 256;

  OpdsServer server;
  std::string feedUrl;
  std::string feedTitle;
  std::string authorizationOrigin;
  std::string ledgerPath;

  freeink::ui::GfxRendererTarget uiTarget;  // must precede `app`: the app holds a reference to it
  UiApp app;
  std::atomic<bool> uiReady{false};
  ScreenTransitionRefresh screenTransitionRefresh;

  Phase phase = Phase::SCANNING;
  StopReason stopReason = StopReason::NONE;
  bool started = false;
  bool cancelRequested = false;
  bool goHomeAfterCancel = false;
  std::string errorMessage;

  // What is already on the card: file name and synced-identity keys.
  opds_sync::KeySet presentKeys;
  // EPUB title/author keys from the Library index, each tagged with its index
  // ordinal so a match can be confirmed against the card before it counts.
  opds_sync::KeySet metadataKeys;
  std::unique_ptr<library::LibraryIndexFile> libraryIndex;
  size_t localBooks = 0;

  // Catalogue walk.
  std::string pageUrl;
  std::string nextPageUrl;
  uint64_t visitedPages[MAX_PAGES] = {};
  size_t pagesFetched = 0;
  size_t catalogueBooks = 0;
  size_t unsupportedEntries = 0;
  size_t missingBooks = 0;
  // Per-attempt counters, committed only when the whole page arrived.
  size_t pageBooks = 0;
  size_t pageMissing = 0;
  bool pageWriteFailed = false;
  HalFile pageFile;
  unsigned long lastProgressRenderMs = 0;

  // Download queue, read sequentially from the SD card.
  std::unique_ptr<char[]> recordBuffer;
  HalFile queueFile;
  std::unique_ptr<LineReader> queueReader;
  size_t queueIndex = 0;
  std::string currentTitle;
  size_t currentProgress = 0;
  size_t currentTotal = 0;
  size_t downloaded = 0;
  size_t failed = 0;
  size_t alreadyPresent = 0;

  static void rootScreen(UiApp::ScreenType& screen, void* user);
  static void onConfirmEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onCancelEvent(const freeink::ui::ActionEvent& event, void* user);
  static bool onCatalogueEntry(void* context, OpdsEntry& entry);

  void buildHeader(UiApp::ScreenType& screen);
  void buildBody(UiApp::ScreenType& screen);
  void addLine(UiApp::ScreenType& screen, const char* text, bool title = false);

  void confirmPressed();
  void cancelPressed();
  bool pollCancel();

  void runScan();
  bool scanDirectory(char* path, size_t pathLen, char* name, int depth);
  void addLibraryIndexKeys();
  bool loadLedger();
  void fetchNextPage();
  bool appendPageToQueue();
  void finishCatalogue();
  void downloadNext();
  bool readQueueRecord(std::string& identity, std::string& url, std::string& title, std::string& author);
  void appendLedger(const std::string& identity, const std::string& path);
  bool isPresent(const OpdsEntry& entry, const std::string& absoluteHref);
  bool metadataPresent(uint64_t key);
  std::string downloadPathFor(const std::string& title, const std::string& author) const;
  void finishWith(Phase next);
  void fail(const char* message);
  void removeWorkFiles();
};
