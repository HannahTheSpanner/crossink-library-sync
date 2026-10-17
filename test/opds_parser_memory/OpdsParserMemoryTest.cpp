// Largest single heap request made while parsing an OPDS page.
//
// On X3/X4 (ESP32-C3, no PSRAM) an HTTPS feed is parsed while WiFi and a TLS
// session hold most of the heap, and the largest free block can be only a few
// KB. A device log showed library sync failing after 2366 bytes of a page
// with OpdsParserError::BUFFER_MEMORY: expat had to grow its input buffer
// from 2 KB to 4 KB. These tests pin the parser to expat's initial 2 KB
// buffer for realistic pages delivered the way SecureHttpClient delivers them
// (up to 2 KB per callback).

#include <gtest/gtest.h>
#include <malloc.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

#include "OpdsParser.h"

extern "C" void* __libc_malloc(size_t);
extern "C" void __libc_free(void*);
extern "C" void* __libc_realloc(void*, size_t);
extern "C" void* __libc_calloc(size_t, size_t);

namespace {
bool measuring = false;
size_t largestRequest = 0;

void record(const size_t size) {
  if (measuring && size > largestRequest) largestRequest = size;
}
}  // namespace

extern "C" void* malloc(size_t size) {
  record(size);
  return __libc_malloc(size);
}
extern "C" void free(void* ptr) { __libc_free(ptr); }
extern "C" void* calloc(size_t count, size_t size) {
  record(count * size);
  return __libc_calloc(count, size);
}
extern "C" void* realloc(void* ptr, size_t size) {
  record(size);
  return __libc_realloc(ptr, size);
}

namespace {

// Deterministic page shaped like Grimmory's (BookLore) OPDS 1.2 acquisition
// feed: 50 entries with HTML descriptions, categories and three links each.
std::string grimmoryPage(const size_t entries) {
  static const char* const words[] = {"the",   "of",    "and",   "light", "river",   "winter", "letters", "house",
                                      "night", "glass", "north", "sea",   "orchard", "storm",  "garden",  "fire"};
  uint32_t seed = 7;
  auto next = [&seed] {
    seed = seed * 1103515245u + 12345u;
    return (seed >> 16) & 0x7fff;
  };
  auto phrase = [&](const size_t count) {
    std::string out;
    for (size_t i = 0; i < count; ++i) {
      if (i) out.push_back(' ');
      out += words[next() % (sizeof(words) / sizeof(words[0]))];
    }
    return out;
  };

  std::string xml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<feed xmlns=\"http://www.w3.org/2005/Atom\" xmlns:dc=\"http://purl.org/dc/terms/\" "
      "xmlns:opds=\"http://opds-spec.org/2010/catalog\">\n"
      "  <id>urn:booklore:catalog:all</id>\n  <title>All Books</title>\n"
      "  <link rel=\"next\" href=\"/api/v1/opds/catalog?page=2&amp;size=50\" "
      "type=\"application/atom+xml;profile=opds-catalog;kind=acquisition\"/>\n";
  char number[48];
  for (size_t i = 0; i < entries; ++i) {
    snprintf(number, sizeof(number), "%zu", 1000 + i);
    xml += "  <entry>\n    <title>" + phrase(2 + next() % 6) + "</title>\n";
    xml += std::string("    <id>urn:booklore:book:") + number + "</id>\n";
    xml += "    <updated>2025-08-01T10:11:12.123456Z</updated>\n";
    xml += "    <author>\n      <name>" + phrase(2) + "</name>\n    </author>\n";
    xml += "    <content type=\"html\">&lt;p&gt;" + phrase(60 + next() % 200) + "&lt;/p&gt;&lt;p&gt;" +
           phrase(20 + next() % 100) + "&lt;/p&gt;</content>\n";
    xml += "    <dc:language>en</dc:language>\n";
    xml += "    <category term=\"Science Fiction &amp; Fantasy\" label=\"Science Fiction &amp; Fantasy\"/>\n";
    xml += std::string("    <link rel=\"http://opds-spec.org/image\" href=\"/api/v1/opds/") + number +
           "/cover?token=abcdef0123456789abcdef0123456789\" type=\"image/jpeg\"/>\n";
    xml += std::string("    <link rel=\"http://opds-spec.org/image/thumbnail\" href=\"/api/v1/opds/") + number +
           "/cover?thumb=true&amp;token=abcdef0123456789abcdef0123456789\" type=\"image/jpeg\"/>\n";
    xml += std::string("    <link rel=\"http://opds-spec.org/acquisition\" href=\"/api/v1/opds/") + number +
           "/download?fileId=5" + number + "\" type=\"application/epub+zip\" title=\"EPUB\"/>\n";
    xml += "  </entry>\n";
  }
  xml += "</feed>\n";
  return xml;
}

size_t streamedBooks = 0;
bool countBooks(void*, OpdsEntry& entry) {
  if (entry.type == OpdsEntryType::BOOK) ++streamedBooks;
  return true;
}

// Feeds `xml` in `delivery`-byte writes and returns the largest single heap
// request made from parser construction to destruction.
template <typename MakeParser>
size_t largestRequestWhileParsing(const std::string& xml, const size_t delivery, MakeParser makeParser,
                                  size_t& parsedEntries) {
  largestRequest = 0;
  measuring = true;
  {
    auto parser = makeParser();
    for (size_t offset = 0; offset < xml.size(); offset += delivery) {
      const size_t length = std::min(delivery, xml.size() - offset);
      parser->write(reinterpret_cast<const uint8_t*>(xml.data()) + offset, length);
      if (parser->error()) break;
    }
    parser->flush();
    measuring = false;
    EXPECT_FALSE(parser->error()) << "reason " << static_cast<int>(parser->getErrorReason());
    parsedEntries = parser->getEntryCount();
  }
  measuring = false;
  return largestRequest;
}

// expat's input buffer starts at 2 KB; nothing else the parser allocates is
// that large.
constexpr size_t EXPAT_INITIAL_BUFFER = 2048;

TEST(OpdsParserMemory, StreamingPageNeverGrowsExpatBuffer) {
  const std::string xml = grimmoryPage(50);
  ASSERT_GT(xml.size(), 60000u);
  for (const size_t delivery : {2048u, 1436u, 1024u, 700u, 97u}) {
    streamedBooks = 0;
    size_t unused = 0;
    const size_t largest = largestRequestWhileParsing(
        xml, delivery, [] { return std::make_unique<OpdsParser>(&countBooks, nullptr); }, unused);
    EXPECT_EQ(streamedBooks, 50u) << "delivery " << delivery;
    EXPECT_LE(largest, EXPAT_INITIAL_BUFFER) << "delivery " << delivery;
  }
}

TEST(OpdsParserMemory, BufferedPageNeverGrowsExpatBuffer) {
  const std::string xml = grimmoryPage(50);
  auto entries = std::make_unique<OpdsEntry[]>(MAX_OPDS_FEED_ENTRIES);
  for (const size_t delivery : {2048u, 1436u}) {
    size_t parsed = 0;
    const size_t largest = largestRequestWhileParsing(
        xml, delivery, [&entries] { return std::make_unique<OpdsParser>(entries.get(), MAX_OPDS_FEED_ENTRIES); },
        parsed);
    EXPECT_EQ(parsed, 50u) << "delivery " << delivery;
    EXPECT_LE(largest, EXPAT_INITIAL_BUFFER) << "delivery " << delivery;
    for (size_t i = 0; i < MAX_OPDS_FEED_ENTRIES; ++i) entries[i] = OpdsEntry{};
  }
}

}  // namespace
