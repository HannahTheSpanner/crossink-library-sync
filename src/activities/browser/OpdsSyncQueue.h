#pragma once

#include <HalStorage.h>
#include <OpdsLibraryMatch.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

/**
 * The SD-card side of a library sync (OpdsLibrarySyncActivity): the records
 * of books to download, and the per-server ledger of what was synced.
 *
 * Three files, all tab-separated text with one record per line:
 * - a page file, rewritten for every catalogue page and appended to the queue
 *   only once the whole page arrived, so a failed page leaves no half-queue;
 * - the queue, read back sequentially while downloading;
 * - the ledger (`identity<TAB>path`), which maps each synced book's OPDS
 *   identity to its file, so a book renamed on the server is not fetched again
 *   and a book deleted from the card is.
 *
 * Nothing here scales with the library: one caller-supplied record buffer
 * serves the ledger load and the queue read, and the keys for what is present
 * go into the caller's KeySet.
 */
class OpdsSyncQueue {
 public:
  // One queue record: identity, link kind, URL (parsers cap at 768), title
  // (160) and author (120) plus separators, with headroom.
  static constexpr size_t RECORD_BUFFER_SIZE = 1280;
  // A 64-bit key as 16 hex digits plus the terminator.
  static constexpr size_t KEY_TEXT_SIZE = 17;

  struct Record {
    std::string identity;  // 16 hex digits
    // True for an OPDS 2 publication document that must be resolved to its
    // EPUB link before downloading; false for a direct EPUB link.
    bool publicationDocument = false;
    std::string url;
    std::string title;
    std::string author;
  };

  // `buffer` holds RECORD_BUFFER_SIZE bytes and outlives this object.
  OpdsSyncQueue(const char* queuePath, const char* pagePath, std::string ledgerPath, char* buffer);
  ~OpdsSyncQueue();
  OpdsSyncQueue(const OpdsSyncQueue&) = delete;
  OpdsSyncQueue& operator=(const OpdsSyncQueue&) = delete;

  static void formatKey(uint64_t key, char (&out)[KEY_TEXT_SIZE]);
  static bool parseKey(std::string_view text, uint64_t& out);

  // --- Ledger -------------------------------------------------------------
  // Adds the identity key of every ledger entry whose file is still on the
  // card to `present`, and rewrites the ledger without the entries whose
  // files have gone. Returns false only when `present` ran out of memory.
  bool loadLedger(opds_sync::KeySet& present, size_t& stale);
  void appendLedger(const std::string& identity, const std::string& path);

  // --- Catalogue walk -----------------------------------------------------
  // Starts a fresh queue for this run.
  void reset();
  // Starts the page file for one catalogue page (an attempt may be retried).
  bool beginPage();
  // Writes one record to the page file. Returns false on a write failure.
  bool appendPageRecord(uint64_t identity, bool publicationDocument, const std::string& url, const std::string& title,
                        const std::string& author);
  // Closes the page file. Returns false if the file did not close cleanly.
  bool endPage();
  // Appends the page file's records to the queue. `records` is how many the
  // page wrote; nothing is opened when it is 0.
  bool commitPage(size_t records);

  // --- Downloads ----------------------------------------------------------
  bool openForReading();
  // Next well-formed record; damaged ones are skipped and counted.
  bool next(Record& record);
  size_t skippedRecords() const { return skipped; }
  void closeReader();

  // Deletes the page and queue files (not the ledger).
  void removeWorkFiles();

 private:
  class LineReader;

  const char* queuePath;
  const char* pagePath;
  std::string ledgerPath;
  char* buffer;
  HalFile pageFile;
  HalFile queueFile;
  std::unique_ptr<LineReader> reader;
  size_t skipped = 0;
};
