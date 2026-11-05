#include <OpdsParser.h>
#include <XmlParserUtils.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr char kMultiAuthorFeed[] = R"(<?xml version="1.0" encoding="UTF-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <entry>
    <title>Book Title</title>
    <id>urn:booklore:book:90</id>
    <author><name>Main Author Name</name></author>
    <author><name>Translator Name</name></author>
    <link href="/api/v1/opds/90/download?fileId=691" rel="http://opds-spec.org/acquisition" type="application/epub+zip" title="EPUB"/>
  </entry>
</feed>)";

// Sanitized from a live Mayberry /opds/branches feed.
constexpr char kSummaryCountFeed[] = R"(<?xml version="1.0" encoding="UTF-8"?>
<feed xmlns="http://www.w3.org/2005/Atom" xmlns:opds="http://opds-spec.org/2010/catalog">
  <id>urn:example:branches</id>
  <title>Branches</title>
  <entry>
    <id>urn:example:branch:a</id>
    <title>Branch A</title>
    <summary type="text">12713 books</summary>
    <link rel="subsection" href="/opds/branch/a" type="application/atom+xml;profile=opds-catalog;kind=acquisition"></link>
  </entry>
  <entry>
    <id>urn:example:branch:b</id>
    <title>Branch B</title>
    <summary type="text">1 books</summary>
    <link rel="subsection" href="/opds/branch/b" type="application/atom+xml;profile=opds-catalog;kind=acquisition"></link>
  </entry>
  <entry>
    <id>urn:example:releases</id>
    <title>New Releases</title>
    <link rel="subsection" href="/opds/releases" type="application/atom+xml;profile=opds-catalog;kind=acquisition"></link>
  </entry>
  <entry>
    <id>urn:example:descr</id>
    <title>Described</title>
    <summary type="text">3 friends set out on a journey</summary>
    <link rel="subsection" href="/opds/descr" type="application/atom+xml;profile=opds-catalog;kind=navigation"></link>
  </entry>
</feed>)";

constexpr char kThrCountFeed[] = R"(<?xml version="1.0" encoding="UTF-8"?>
<feed xmlns="http://www.w3.org/2005/Atom" xmlns:thr="http://purl.org/syndication/thread/1.0">
  <entry>
    <title>Fiction</title>
    <summary>99 books</summary>
    <link rel="subsection" href="/fiction" type="application/atom+xml;profile=opds-catalog" thr:count="42"/>
  </entry>
  <entry>
    <title>A Book</title>
    <summary>5 stars</summary>
    <link href="/b.epub" rel="http://opds-spec.org/acquisition" type="application/epub+zip"/>
  </entry>
</feed>)";

}  // namespace

TEST(OpdsParserTest, MultipleAuthorsKeepFirstAuthor) {
  OpdsEntry entries[MAX_OPDS_FEED_ENTRIES];
  OpdsParser parser(entries);

  ASSERT_TRUE(parser.parse(kMultiAuthorFeed, sizeof(kMultiAuthorFeed) - 1));
  ASSERT_EQ(parser.getEntryCount(), 1u);
  ASSERT_NE(parser.getEntry(0), nullptr);
  EXPECT_EQ(parser.getEntry(0)->author, "Main Author Name");
}

TEST(OpdsParserTest, NavigationCountFromSummary) {
  OpdsEntry entries[MAX_OPDS_FEED_ENTRIES];
  OpdsParser parser(entries);

  ASSERT_TRUE(parser.parse(kSummaryCountFeed, sizeof(kSummaryCountFeed) - 1));
  ASSERT_EQ(parser.getEntryCount(), 4u);
  EXPECT_EQ(parser.getEntry(0)->count, 12713);
  EXPECT_EQ(parser.getEntry(1)->count, 1);
  EXPECT_EQ(parser.getEntry(2)->count, -1);
  EXPECT_EQ(parser.getEntry(3)->count, -1);
}

TEST(OpdsParserTest, NavigationCountFromIndentedSummary) {
  constexpr char feed[] = R"(<?xml version="1.0" encoding="UTF-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <entry>
    <id>urn:example:indented</id>
    <title>Indented</title>
    <summary type="text">
                        12713 books
                    </summary>
    <link rel="subsection" href="/opds/a" type="application/atom+xml;profile=opds-catalog;kind=acquisition"></link>
  </entry>
  <entry>
    <id>urn:example:sentence</id>
    <title>Sentence</title>
    <summary type="text">
        3 books      that were picked by the staff this week
    </summary>
    <link rel="subsection" href="/opds/b" type="application/atom+xml;profile=opds-catalog;kind=acquisition"></link>
  </entry>
</feed>)";
  OpdsEntry entries[MAX_OPDS_FEED_ENTRIES];
  OpdsParser parser(entries);

  ASSERT_TRUE(parser.parse(feed, sizeof(feed) - 1));
  ASSERT_EQ(parser.getEntryCount(), 2u);
  EXPECT_EQ(parser.getEntry(0)->count, 12713);
  EXPECT_EQ(parser.getEntry(1)->count, -1);
}

TEST(OpdsParserTest, ThrCountWinsAndBooksHaveNoCount) {
  OpdsEntry entries[MAX_OPDS_FEED_ENTRIES];
  OpdsParser parser(entries);

  ASSERT_TRUE(parser.parse(kThrCountFeed, sizeof(kThrCountFeed) - 1));
  ASSERT_EQ(parser.getEntryCount(), 2u);
  EXPECT_EQ(parser.getEntry(0)->type, OpdsEntryType::NAVIGATION);
  EXPECT_EQ(parser.getEntry(0)->count, 42);
  EXPECT_EQ(parser.getEntry(1)->type, OpdsEntryType::BOOK);
  EXPECT_EQ(parser.getEntry(1)->count, -1);
}

TEST(OpdsParserTest, CountBoundsAndUnrelatedSummaries) {
  const char* values[] = {"0", "2147483647", "2147483648", "-1", "12x", ""};
  const int32_t expected[] = {0, INT32_MAX, -1, -1, -1, -1};
  for (size_t i = 0; i < 6; ++i) {
    const std::string feed =
        "<feed xmlns:t='http://purl.org/syndication/thread/1.0'><entry><title>Category</title>"
        "<link href='/a' type='application/atom+xml' t:count='" +
        std::string(values[i]) + "'/><summary>1984 classics</summary></entry></feed>";
    OpdsEntry entries[1];
    OpdsParser parser(entries, 1);
    // Exercise text split at arbitrary network boundaries as well as parser reuse.
    for (const unsigned char ch : feed) parser.write(ch);
    parser.flush();
    ASSERT_FALSE(parser.error());
    ASSERT_EQ(parser.getEntryCount(), 1u);
    EXPECT_EQ(entries[0].count, expected[i]);
    constexpr char emptyFeed[] =
        "<feed><entry><title>Empty</title><link href='/b' type='application/atom+xml'/></entry></feed>";
    ASSERT_TRUE(parser.parse(emptyFeed, sizeof(emptyFeed) - 1));
    EXPECT_EQ(entries[0].count, -1);
  }
}

namespace {
std::string descriptionFeed(const std::string& body) {
  return "<feed><entry><title>Original</title><author><name>Author</name></author>" + body +
         "<link href='/book.epub' rel='http://opds-spec.org/acquisition' type='application/epub+zip'/></entry></feed>";
}
}  // namespace

TEST(OpdsParserTest, DescriptionMarkupCannotOverwriteEntryFields) {
  OpdsEntry entries[1];
  OpdsParser parser(entries);
  const auto xml = descriptionFeed(
      "<summary type='xhtml'><div><p>First</p><p>Second <title>inside</title> "
      "<link href='/wrong'/></p></div></summary>");
  ASSERT_TRUE(parser.parse(xml.c_str(), xml.size()));
  EXPECT_STREQ(entries[0].description.data(), "First Second inside");
  EXPECT_EQ(entries[0].title, "Original");
  EXPECT_EQ(entries[0].href, "/book.epub");
}

TEST(OpdsParserTest, ContentPreferredRegardlessOfOrder) {
  for (const auto* body : {"<summary>Short</summary><content>Full description</content>",
                           "<content>Full description</content><summary>Short</summary>",
                           "<content>Full description</content><content></content>"}) {
    OpdsEntry entries[1];
    OpdsParser parser(entries);
    const auto xml = descriptionFeed(body);
    ASSERT_TRUE(parser.parse(xml.c_str(), xml.size()));
    EXPECT_STREQ(entries[0].description.data(), "Full description");
  }
}

TEST(OpdsParserTest, EscapedHtmlAcrossSingleByteChunks) {
  OpdsEntry entries[1];
  OpdsParser parser(entries);
  const auto xml =
      descriptionFeed("<summary type='html'>&lt;p&gt;One&lt;/p&gt;&lt;p&gt;Two &amp; three&lt;/p&gt;</summary>");
  for (const auto c : xml) parser.write(static_cast<uint8_t>(c));
  parser.flush();
  ASSERT_FALSE(parser.error());
  EXPECT_STREQ(entries[0].description.data(), "One Two & three");
}

TEST(OpdsParserTest, PlainTextKeepsAngleBrackets) {
  OpdsEntry entries[1];
  OpdsParser parser(entries);
  const auto xml = descriptionFeed("<summary>2 &lt; 3 and 5 &gt; 4</summary>");
  ASSERT_TRUE(parser.parse(xml.c_str(), xml.size()));
  EXPECT_STREQ(entries[0].description.data(), "2 < 3 and 5 > 4");
}

TEST(OpdsParserTest, DescriptionTruncatesAtUtf8BoundaryAndResets) {
  OpdsEntry entries[1];
  OpdsParser parser(entries);
  for (size_t prefix = 250; prefix < 257; ++prefix) {
    const auto xml = descriptionFeed("<summary>" + std::string(prefix, 'a') + "漢字 description</summary>");
    ASSERT_TRUE(parser.parse(xml.c_str(), xml.size()));
    const std::string out = entries[0].description.data();
    EXPECT_LE(out.size(), MAX_OPDS_DESCRIPTION_BYTES);
    ASSERT_GE(out.size(), 3u);
    EXPECT_EQ(out.substr(out.size() - 3), "...");
    for (size_t i = 0; i < out.size(); ++i) {
      const auto c = static_cast<unsigned char>(out[i]);
      if (c >= 0x80) {
        ASSERT_EQ(out.substr(i, 3), "漢");
        i += 2;
      }
    }
  }
  const auto empty = descriptionFeed("");
  ASSERT_TRUE(parser.parse(empty.c_str(), empty.size()));
  EXPECT_STREQ(entries[0].description.data(), "");
}

TEST(OpdsParserTest, NonTextContentDoesNotReplaceSummary) {
  OpdsEntry entries[1];
  OpdsParser parser(entries);
  const auto xml = descriptionFeed("<summary>A description</summary><content type='application/pdf'>BASE64</content>");
  ASSERT_TRUE(parser.parse(xml.c_str(), xml.size()));
  EXPECT_STREQ(entries[0].description.data(), "A description");
}

TEST(OpdsParserTest, LongDescriptionsDoNotInflateFollowingBookMetadata) {
  const std::string entry =
      "<entry><title>A recent book with a short title</title>"
      "<author><name>Example Author</name></author><id>urn:example:book:123456789</id>"
      "<link href='/api/v1/opds/123/download?fileId=456' "
      "rel='http://opds-spec.org/acquisition' type='application/epub+zip'/>"
      "<summary>" +
      std::string(1700, 'd') + "</summary></entry>";
  std::string xml = "<feed>";
  for (size_t i = 0; i < MAX_OPDS_FEED_ENTRIES; ++i) xml += entry;
  xml += "<link rel='next' href='/recent?page=2'/></feed>";
  for (const size_t chunkSize : {1u, 37u, 1024u, 2048u}) {
    SCOPED_TRACE(chunkSize);
    OpdsEntry entries[MAX_OPDS_FEED_ENTRIES];
    OpdsParser parser(entries);
    // Network splits must not change the retained metadata allocation.
    for (size_t offset = 0; offset < xml.size(); offset += chunkSize) {
      ASSERT_EQ(
          parser.write(reinterpret_cast<const uint8_t*>(xml.data() + offset), std::min(chunkSize, xml.size() - offset)),
          std::min(chunkSize, xml.size() - offset));
    }
    parser.flush();
    ASSERT_FALSE(parser.error());
    ASSERT_EQ(parser.getEntryCount(), MAX_OPDS_FEED_ENTRIES);
    EXPECT_EQ(parser.getNextPageUrl(), "/recent?page=2");
    size_t retainedCapacity = 0;
    for (const auto& book : parser.getEntries()) {
      EXPECT_EQ(book.title, "A recent book with a short title");
      EXPECT_EQ(book.author, "Example Author");
      EXPECT_EQ(book.id, "urn:example:book:123456789");
      EXPECT_EQ(book.href, "/api/v1/opds/123/download?fileId=456");
      EXPECT_EQ(std::strlen(book.description.data()), MAX_OPDS_DESCRIPTION_BYTES);
      retainedCapacity += book.title.capacity() + book.author.capacity() + book.id.capacity() + book.href.capacity();
    }
    // Excludes the fixed entry/description array and Expat/Wi-Fi. In particular,
    // a short title must not inherit the previous entry's 256-byte preview scratch.
    std::printf("OPDS %zu-byte feed, chunk=%zu: retained metadata capacity=%zu bytes\n", xml.size(), chunkSize,
                retainedCapacity);
    EXPECT_LT(retainedCapacity, 10 * 1024u);
  }
}

TEST(OpdsParserTest, RejectsChunkImmediatelyOnXmlErrorAndCanRetry) {
  OpdsEntry entries[1];
  OpdsParser parser(entries);
  constexpr char invalid[] = "<feed><entry></feed>";
  EXPECT_EQ(parser.write(reinterpret_cast<const uint8_t*>(invalid), sizeof(invalid) - 1), 0u);
  EXPECT_EQ(parser.getErrorReason(), OpdsParserError::XML_PARSE);
  EXPECT_EQ(parser.write('x'), 0u);
  parser.flush();
  EXPECT_EQ(parser.getErrorReason(), OpdsParserError::XML_PARSE);
  ASSERT_TRUE(parser.parse(kMultiAuthorFeed, sizeof(kMultiAuthorFeed) - 1));
  EXPECT_EQ(parser.getEntryCount(), 1u);
}

TEST(OpdsParserTest, RejectsChunkImmediatelyOnBufferAllocationFailureAndCanRetry) {
  OpdsEntry entries[1];
  OpdsParser parser(entries);
  ASSERT_GT(XmlTestHeap::live, 0u);
  XmlTestHeap::limit = XmlTestHeap::live;
  EXPECT_EQ(parser.write('<'), 0u);
  EXPECT_EQ(parser.getErrorReason(), OpdsParserError::BUFFER_MEMORY);
  EXPECT_EQ(XmlTestHeap::live, 0u);
  EXPECT_EQ(parser.write('x'), 0u);
  parser.flush();
  EXPECT_EQ(parser.getErrorReason(), OpdsParserError::BUFFER_MEMORY);
  XmlTestHeap::limit = SIZE_MAX;
  ASSERT_TRUE(parser.parse(kMultiAuthorFeed, sizeof(kMultiAuthorFeed) - 1));
  EXPECT_EQ(parser.getEntryCount(), 1u);
}

TEST(OpdsParserTest, RejectsWritesWhenParserCannotBeAllocated) {
  OpdsEntry entries[1];
  XmlTestHeap::limit = 0;
  {
    OpdsParser parser(entries);
    EXPECT_EQ(parser.getErrorReason(), OpdsParserError::PARSER_MEMORY);
    EXPECT_EQ(parser.write('<'), 0u);
    EXPECT_EQ(XmlTestHeap::live, 0u);
  }
  XmlTestHeap::limit = SIZE_MAX;
}

namespace {

struct SinkLog {
  std::vector<OpdsEntry> entries;
  size_t stopAfter = 0;  // 0 = never stop
};

bool collectEntry(void* context, OpdsEntry& entry) {
  auto* log = static_cast<SinkLog*>(context);
  log->entries.push_back(std::move(entry));
  return log->stopAfter == 0 || log->entries.size() < log->stopAfter;
}

std::string bookFeed(const size_t books, const bool withNext) {
  std::string feed =
      "<?xml version='1.0'?><feed xmlns='http://www.w3.org/2005/Atom' "
      "xmlns:opensearch='http://a9.com/-/spec/opensearch/1.1/'>";
  if (withNext) feed += "<link rel='next' href='/api/v1/opds/catalog?page=2' type='application/atom+xml'/>";
  for (size_t i = 0; i < books; ++i) {
    const std::string n = std::to_string(i);
    feed += "<entry><title>Book " + n + "</title><id>urn:booklore:book:" + n + "</id><author><name>Author " + n +
            "</name></author><link href='/api/v1/opds/" + n +
            "/download' rel='http://opds-spec.org/acquisition' type='application/epub+zip'/></entry>";
  }
  // A PDF-only book: no EPUB acquisition link, so it is seen but not delivered.
  feed +=
      "<entry><title>Scan</title><id>urn:booklore:book:pdf</id><link href='/api/v1/opds/pdf/download' "
      "rel='http://opds-spec.org/acquisition' type='application/pdf'/></entry>";
  feed += "</feed>";
  return feed;
}

}  // namespace

TEST(OpdsParserTest, SinkModeStreamsPagesLargerThanTheEntryBuffer) {
  // A real "All books" page can exceed MAX_OPDS_FEED_ENTRIES; the sink must see every one.
  const std::string feed = bookFeed(MAX_OPDS_FEED_ENTRIES * 3, true);
  SinkLog log;
  OpdsParser parser(&collectEntry, &log);
  // Feed it in uneven network-sized chunks.
  for (size_t pos = 0; pos < feed.size(); pos += 700) {
    const size_t len = std::min<size_t>(700, feed.size() - pos);
    parser.write(reinterpret_cast<const uint8_t*>(feed.data() + pos), len);
  }
  parser.flush();

  ASSERT_FALSE(parser.error());
  ASSERT_EQ(log.entries.size(), MAX_OPDS_FEED_ENTRIES * 3);
  EXPECT_EQ(parser.getEntryCount(), 0u);
  EXPECT_EQ(parser.getEntriesSeen(), MAX_OPDS_FEED_ENTRIES * 3 + 1);
  EXPECT_FALSE(parser.wasTruncated());
  EXPECT_EQ(parser.getNextPageUrl(), "/api/v1/opds/catalog?page=2");
  EXPECT_EQ(log.entries[0].type, OpdsEntryType::BOOK);
  EXPECT_EQ(log.entries[0].title, "Book 0");
  EXPECT_EQ(log.entries[0].author, "Author 0");
  EXPECT_EQ(log.entries[0].id, "urn:booklore:book:0");
  EXPECT_EQ(log.entries[149].href, "/api/v1/opds/149/download");
}

TEST(OpdsParserTest, SinkCanStopTheParse) {
  const std::string feed = bookFeed(10, false);
  SinkLog log;
  log.stopAfter = 3;
  OpdsParser parser(&collectEntry, &log);
  parser.write(reinterpret_cast<const uint8_t*>(feed.data()), feed.size());
  parser.flush();

  EXPECT_TRUE(parser.error());
  EXPECT_EQ(parser.getErrorReason(), OpdsParserError::SINK_ABORTED);
  EXPECT_EQ(log.entries.size(), 3u);
}

TEST(OpdsParserTest, SinkModeParserIsReusableAfterClear) {
  SinkLog log;
  OpdsParser parser(&collectEntry, &log);
  const std::string first = bookFeed(2, true);
  ASSERT_TRUE(parser.parse(first.data(), first.size()));
  EXPECT_EQ(parser.getEntriesSeen(), 3u);
  const std::string second = bookFeed(1, false);
  ASSERT_TRUE(parser.parse(second.data(), second.size()));
  EXPECT_EQ(parser.getEntriesSeen(), 2u);
  EXPECT_TRUE(parser.getNextPageUrl().empty());
  EXPECT_EQ(log.entries.size(), 3u);
}

TEST(OpdsParserTest, SinkModeCountsNavigationEntriesSeparately) {
  // A mixed root page: two sub-catalogues, one book and a PDF-only book. Only
  // the PDF should read as an unusable book to a caller.
  std::string feed = "<?xml version='1.0'?><feed xmlns='http://www.w3.org/2005/Atom'>";
  feed +=
      "<entry><title>New</title><id>nav:new</id><link href='/new' rel='subsection' "
      "type='application/atom+xml;profile=opds-catalog;kind=acquisition'/></entry>";
  feed +=
      "<entry><title>Authors</title><id>nav:authors</id><link href='/authors' rel='subsection' "
      "type='application/atom+xml;profile=opds-catalog;kind=navigation'/></entry>";
  feed +=
      "<entry><title>Book</title><id>b:1</id><link href='/1.epub' rel='http://opds-spec.org/acquisition' "
      "type='application/epub+zip'/></entry>";
  feed +=
      "<entry><title>Scan</title><id>b:2</id><link href='/2.pdf' rel='http://opds-spec.org/acquisition' "
      "type='application/pdf'/></entry></feed>";
  SinkLog log;
  OpdsParser parser(&collectEntry, &log);
  ASSERT_TRUE(parser.parse(feed.data(), feed.size()));
  EXPECT_EQ(parser.getEntriesSeen(), 4u);
  EXPECT_EQ(parser.getNavigationEntriesSeen(), 2u);
  ASSERT_EQ(log.entries.size(), 3u);
  EXPECT_EQ(log.entries[2].type, OpdsEntryType::BOOK);
  EXPECT_EQ(parser.getEntriesSeen() - parser.getNavigationEntriesSeen() - 1, 1u);  // the PDF
}

TEST(OpdsParserTest, EntryResetRestoresDefaultsAndKeepsNothing) {
  OpdsEntry entry;
  entry.type = OpdsEntryType::BOOK;
  entry.title = "T";
  entry.author = "A";
  entry.href = "h";
  entry.id = "i";
  entry.count = 7;
  entry.description[0] = 'd';
  entry.selfHref = "s";
  entry.indirect = entry.unsupportedAcquisition = true;
  entry.reset();
  const OpdsEntry fresh;
  EXPECT_EQ(entry.type, fresh.type);
  EXPECT_TRUE(entry.title.empty() && entry.author.empty() && entry.href.empty() && entry.id.empty());
  EXPECT_EQ(entry.count, fresh.count);
  EXPECT_EQ(entry.description[0], '\0');
  EXPECT_TRUE(entry.selfHref.empty());
  EXPECT_FALSE(entry.indirect);
  EXPECT_FALSE(entry.unsupportedAcquisition);
}

TEST(OpdsParserTest, NullSinkIsAnError) {
  OpdsParser parser(static_cast<OpdsEntrySink>(nullptr), nullptr);
  EXPECT_TRUE(parser.error());
}
