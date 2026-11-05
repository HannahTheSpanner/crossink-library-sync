#pragma once

#include <Memory.h>

#include <memory>

#include "OpdsJsonParser.h"

// Sniff the response, then allocate only the selected parser. Both formats
// write into the browser's existing bounded, fallibly allocated entry array.
class OpdsFeedParser final : public Print {
 public:
  OpdsFeedParser(OpdsEntry* entries, size_t capacity) : entries(entries), capacity(capacity) {}
  // Streaming mode: whichever format the response turns out to be, entries go
  // to `sink` as they complete (see OpdsParser and OpdsJsonParser).
  OpdsFeedParser(OpdsEntrySink sink, void* sinkContext) : sink(sink), sinkContext(sinkContext) {}
  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* bytes, size_t length) override;
  void flush() override;
  explicit operator bool() const { return getErrorReason() == OpdsParserError::NONE; }
  OpdsParserError getErrorReason() const;
  size_t getEntryCount() const { return xml ? xml->getEntryCount() : json ? json->getEntryCount() : 0; }
  bool wasTruncated() const { return xml ? xml->wasTruncated() : json && json->truncated(); }
  // Entries seen in the page, including unusable ones (see the parsers).
  size_t getEntriesSeen() const { return xml ? xml->getEntriesSeen() : json ? json->getEntriesSeen() : 0; }
  // Navigation entries among those seen. OPDS 2 counts publications only, so
  // its navigation rows are never part of getEntriesSeen().
  size_t getNavigationEntriesSeen() const { return xml ? xml->getNavigationEntriesSeen() : 0; }
  // An OPDS 2 page whose publications sit inside groups (previews of other feeds).
  bool hasGroupedPublications() const { return json && json->grouped(); }
  const std::string& getSearchTemplate() const;
  const std::string& getNextPageUrl() const;
  const std::string& getPrevPageUrl() const;
  void setPreferredLanguage(const char* language);

 private:
  OpdsEntry* entries = nullptr;
  size_t capacity = 0;
  OpdsEntrySink sink = nullptr;
  void* sinkContext = nullptr;
  std::unique_ptr<OpdsParser> xml;
  std::unique_ptr<OpdsJsonParser> json;
  OpdsParserError failure = OpdsParserError::NONE;
  uint8_t bomBytes = 0;
  char language[12] = {};
};
