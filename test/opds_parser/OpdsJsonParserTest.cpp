#include <OpdsFeedParser.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {
class OpdsJsonTest : public ::testing::Test {
 protected:
  OpdsEntry entries[4];
  bool feed(OpdsFeedParser& parser, const std::string& text) {
    for (const auto byte : text)
      if (parser.write(static_cast<uint8_t>(byte)) != 1) return false;
    parser.flush();
    return bool(parser);
  }
};
TEST_F(OpdsJsonTest, SniffsBothFormatsAndKeepsDescriptionsAndPaging) {
  OpdsFeedParser parser(entries, 4);
  parser.setPreferredLanguage("en");
  ASSERT_TRUE(feed(
      parser,
      "\xEF\xBB\xBF \n"
      R"({"publications":[{"metadata":{"title":{"fr":"Livre","en":"Book"},"author":[{"name":"Writer"}],"description":"<p>A story</p>"},"links":[{"href":"sample.epub","type":"application/epub+zip","rel":"http://opds-spec.org/acquisition/sample"},{"href":"book.epub","type":"application/epub+zip","rel":["self","http://opds-spec.org/acquisition/open-access"]}]}],"links":[{"href":"?page=2","rel":"next"},{"href":"?page=0","rel":"previous"},{"href":"?q={searchTerms}","rel":"search","templated":true}]})"));
  ASSERT_EQ(parser.getEntryCount(), 1u);
  EXPECT_EQ(entries[0].title, "Book");
  EXPECT_EQ(entries[0].author, "Writer");
  EXPECT_STREQ(entries[0].description.data(), "A story ");
  EXPECT_EQ(entries[0].href, "book.epub");
  EXPECT_EQ(parser.getNextPageUrl(), "?page=2");
  EXPECT_EQ(parser.getPrevPageUrl(), "?page=0");
  EXPECT_EQ(parser.getSearchTemplate(), "?q={searchTerms}");
  OpdsFeedParser xml(entries, 4);
  ASSERT_TRUE(
      feed(xml, "<feed><entry><title>Folder</title><link href='next' type='application/atom+xml'/></entry></feed>"));
  EXPECT_EQ(xml.getEntryCount(), 1u);
}
TEST_F(OpdsJsonTest, BoundsEntriesButReadsTrailingNextLink) {
  OpdsFeedParser parser(entries, 1);
  ASSERT_TRUE(feed(
      parser,
      R"({"navigation":[{"title":"One","href":"one"},{"title":"Two","href":"two"}],"links":[{"rel":"next","href":"next"}]})"));
  EXPECT_EQ(parser.getEntryCount(), 1u);
  EXPECT_TRUE(parser.wasTruncated());
  EXPECT_EQ(parser.getNextPageUrl(), "next");
  EXPECT_TRUE(entries[1].title.empty());
}
TEST_F(OpdsJsonTest, GroupsAndFacetsRemainNavigable) {
  OpdsFeedParser parser(entries, 4);
  ASSERT_TRUE(feed(
      parser,
      R"({"groups":[{"metadata":{"title":"All stories"},"links":[{"rel":"self","href":"stories"}],"navigation":[{"title":"Category","href":"category"}]}],"facets":[{"links":[{"title":"English","href":"en","properties":{"numberOfItems":42}}]}]})"));
  ASSERT_EQ(parser.getEntryCount(), 3u);
  EXPECT_EQ(entries[1].title, "All stories");
  EXPECT_EQ(entries[1].href, "stories");
  EXPECT_EQ(entries[2].count, 42);
}
TEST_F(OpdsJsonTest, RejectsTruncatedAndInvalidDocuments) {
  for (const std::string text : {"", "{", "{}{}", "[1]", "oops", "{\"navigation\":[{\"title\":\"unfinished"}) {
    OpdsFeedParser parser(entries, 4);
    EXPECT_FALSE(feed(parser, text)) << text;
  }
}
TEST_F(OpdsJsonTest, StandalonePublicationAndIndirectLinks) {
  OpdsJsonParser parser(entries, 1, true);
  const std::string doc =
      R"({"metadata":{"title":"Details","description":"Full description"},"links":[{"rel":"self","type":"application/opds-publication+json","href":"details.json"},{"rel":"acquisition","type":"application/opds-publication+json","href":"fulfill.json"}]})";
  ASSERT_EQ(parser.write(reinterpret_cast<const uint8_t*>(doc.data()), doc.size()), doc.size());
  parser.flush();
  ASSERT_FALSE(parser.error());
  ASSERT_EQ(parser.getEntryCount(), 1u);
  EXPECT_TRUE(entries[0].indirect);
  EXPECT_EQ(entries[0].selfHref, "details.json");
  EXPECT_EQ(entries[0].href, "fulfill.json");
}
TEST_F(OpdsJsonTest, DoesNotTreatLcpOrPurchasesAsEpubDownloads) {
  OpdsFeedParser parser(entries, 4);
  ASSERT_TRUE(feed(
      parser,
      R"({"publications":[{"metadata":{"title":"Loan"},"links":[{"rel":"borrow","type":"application/vnd.readium.lcp.license.v1.0+json","href":"loan.lcpl"}]},{"metadata":{"title":"Purchase"},"links":[{"rel":"buy","type":"application/epub+zip","href":"buy"}]}]})"));
  ASSERT_EQ(parser.getEntryCount(), 2u);
  EXPECT_TRUE(entries[0].unsupportedAcquisition);
  EXPECT_TRUE(entries[1].unsupportedAcquisition);
}
TEST_F(OpdsJsonTest, DescriptionDoesNotSplitUtf8OrOverflowCallerStorage) {
  OpdsFeedParser parser(entries, 4);
  std::string description(255, 'a');
  description += "\xC3\xA9";
  ASSERT_TRUE(
      feed(parser,
           "{\"publications\":[{\"metadata\":{\"title\":\"Book\",\"description\":\"" + description +
               "\"},\"links\":[{\"rel\":\"download\",\"type\":\"application/epub+zip\",\"href\":\"book.epub\"}]}]}"));
  EXPECT_EQ(strlen(entries[0].description.data()), 255u);
}
TEST_F(OpdsJsonTest, KeepsExcerptWhenDescriptionExceedsTokenizerBuffer) {
  OpdsFeedParser parser(entries, 4);
  const std::string text =
      "{\"publications\":[{\"metadata\":{\"title\":\"Book\",\"description\":\"" + std::string(6000, 'd') +
      "\"},\"links\":[{\"rel\":\"download\",\"type\":\"application/epub+zip\",\"href\":\"book.epub\"}]}]}";
  ASSERT_TRUE(feed(parser, text));
  ASSERT_EQ(parser.getEntryCount(), 1u);
  EXPECT_EQ(std::string(entries[0].description.data()), std::string(MAX_OPDS_DESCRIPTION_BYTES, 'd'));
  EXPECT_EQ(entries[0].href, "book.epub");
}
TEST_F(OpdsJsonTest, BoundsCombinedStringsAndRetainsPagination) {
  OpdsEntry largeEntries[50];
  OpdsFeedParser parser(largeEntries, 50);
  std::string json = "{\"navigation\":[";
  for (int i = 0; i < 50; ++i) {
    if (i) json += ',';
    json += "{\"title\":\"" + std::string(160, 't') + "\",\"href\":\"" + std::string(768, 'u') + "\"}";
  }
  json += "],\"links\":[{\"rel\":\"next\",\"href\":\"next\"}]}";
  ASSERT_TRUE(feed(parser, json));
  EXPECT_GT(parser.getEntryCount(), 0u);
  EXPECT_LT(parser.getEntryCount(), 50u);
  EXPECT_TRUE(parser.wasTruncated());
  EXPECT_EQ(parser.getNextPageUrl(), "next");
  size_t bytes = 0;
  for (size_t i = 0; i < parser.getEntryCount(); ++i)
    bytes += largeEntries[i].title.size() + largeEntries[i].href.size();
  EXPECT_LE(bytes, 24u * 1024u);
}
TEST_F(OpdsJsonTest, SupportedAcquisitionWinsInEitherOrder) {
  const std::string epub = R"({"rel":"acquisition","type":"application/epub+zip","href":"book.epub"})";
  const std::string license =
      R"({"rel":"open-access","type":"application/vnd.readium.lcp.license.v1.0+json","href":"license"})";
  for (const auto& links : {epub + "," + license, license + "," + epub}) {
    OpdsFeedParser parser(entries, 4);
    ASSERT_TRUE(feed(parser, R"({"publications":[{"metadata":{"title":"Book"},"links":[)" + links + "]}]}"));
    ASSERT_EQ(parser.getEntryCount(), 1u);
    EXPECT_EQ(entries[0].href, "book.epub");
    EXPECT_FALSE(entries[0].unsupportedAcquisition);
  }
}

// ---- Streaming (library sync) mode ----

struct StreamLog {
  std::vector<OpdsEntry> books;
  size_t navigation = 0;
  size_t stopAfterBooks = 0;  // 0 = never stop
};

bool streamEntry(void* context, OpdsEntry& entry) {
  auto* log = static_cast<StreamLog*>(context);
  if (entry.type != OpdsEntryType::BOOK) {
    ++log->navigation;
    return true;
  }
  log->books.push_back(std::move(entry));
  return log->stopAfterBooks == 0 || log->books.size() < log->stopAfterBooks;
}

bool feedChunks(OpdsFeedParser& parser, const std::string& text, const size_t chunk) {
  for (size_t pos = 0; pos < text.size(); pos += chunk) {
    const size_t n = std::min(chunk, text.size() - pos);
    if (parser.write(reinterpret_cast<const uint8_t*>(text.data() + pos), n) != n) return false;
  }
  parser.flush();
  return bool(parser);
}

std::string jsonPublication(const size_t i) {
  const std::string n = std::to_string(i);
  return R"({"metadata":{"identifier":"urn:uuid:)" + n + R"(","title":"Book )" + n + R"(","author":"Writer )" + n +
         R"("},"links":[{"rel":"http://opds-spec.org/acquisition","type":"application/epub+zip","href":"/books/)" + n +
         R"(.epub"}]})";
}

TEST(OpdsFeedStreamTest, JsonPageLargerThanTheEntryBufferReachesTheSink) {
  std::string doc = R"({"metadata":{"title":"All"},"links":[{"rel":"next","href":"?page=2"}],"publications":[)";
  const size_t books = MAX_OPDS_FEED_ENTRIES * 3;
  for (size_t i = 0; i < books; ++i) doc += (i ? "," : "") + jsonPublication(i);
  doc += "]}";
  StreamLog log;
  OpdsFeedParser parser(&streamEntry, &log);
  ASSERT_TRUE(feedChunks(parser, doc, 700));
  ASSERT_EQ(log.books.size(), books);
  EXPECT_EQ(parser.getEntriesSeen(), books);
  EXPECT_EQ(log.books[0].title, "Book 0");
  EXPECT_EQ(log.books[0].author, "Writer 0");
  EXPECT_EQ(log.books[books - 1].href, "/books/" + std::to_string(books - 1) + ".epub");
  EXPECT_EQ(parser.getNextPageUrl(), "?page=2");
  EXPECT_FALSE(parser.hasGroupedPublications());
}

TEST(OpdsFeedStreamTest, JsonGroupPreviewsAreNotPartOfThePage) {
  const std::string doc =
      R"({"navigation":[{"title":"All books","href":"/all"}],"groups":[{"metadata":{"title":"New"},)"
      R"("links":[{"rel":"self","href":"/new"}],"publications":[)" +
      jsonPublication(1) + "," + jsonPublication(2) + R"(]}],"publications":[)" + jsonPublication(3) + "]}";
  StreamLog log;
  OpdsFeedParser parser(&streamEntry, &log);
  ASSERT_TRUE(feedChunks(parser, doc, 13));
  ASSERT_EQ(log.books.size(), 1u);
  EXPECT_EQ(log.books[0].title, "Book 3");
  EXPECT_EQ(parser.getEntriesSeen(), 1u);  // previews are neither delivered nor "unsupported"
  EXPECT_TRUE(parser.hasGroupedPublications());
  EXPECT_EQ(log.navigation, 2u);  // "All books" and the group's "see all" row

  // The browser's bounded mode still lists the previews and reports the grouping.
  OpdsEntry entries[8];
  OpdsFeedParser browse(entries, 8);
  ASSERT_TRUE(feedChunks(browse, doc, 13));
  EXPECT_TRUE(browse.hasGroupedPublications());
}

TEST(OpdsFeedStreamTest, JsonUnusablePublicationsAreSeenButNotDelivered) {
  const std::string doc =
      R"({"publications":[)" + jsonPublication(1) +
      R"(,{"metadata":{"title":"Loan"},"links":[{"rel":"borrow","type":"application/vnd.readium.lcp.license.v1.0+json","href":"loan.lcpl"}]})"
      R"(,{"metadata":{"title":"Purchase"},"links":[{"rel":"buy","type":"application/epub+zip","href":"buy"}]})"
      R"(,{"metadata":{"title":"Scan"},"links":[{"rel":"acquisition","type":"application/pdf","href":"scan.pdf"}]})"
      R"(,{"metadata":{"title":"Fulfil"},"links":[{"rel":"acquisition","type":"application/opds-publication+json","href":"fulfil.json"}]})"
      R"(,{"metadata":{"title":"Details only"},"links":[{"rel":"self","type":"application/opds-publication+json","href":"details.json"}]})"
      R"(]})";
  StreamLog log;
  OpdsFeedParser parser(&streamEntry, &log);
  ASSERT_TRUE(feedChunks(parser, doc, 64));
  EXPECT_EQ(parser.getEntriesSeen(), 6u);
  // Delivered: the plain EPUB, the indirect acquisition and the details-only
  // publication (both resolved through their publication documents later).
  ASSERT_EQ(log.books.size(), 3u);
  EXPECT_EQ(log.books[0].href, "/books/1.epub");
  EXPECT_FALSE(log.books[0].indirect);
  EXPECT_EQ(log.books[1].title, "Fulfil");
  EXPECT_TRUE(log.books[1].indirect);
  EXPECT_EQ(log.books[2].title, "Details only");
  EXPECT_TRUE(log.books[2].href.empty());
  EXPECT_EQ(log.books[2].selfHref, "details.json");
  for (const auto& book : log.books) EXPECT_FALSE(book.unsupportedAcquisition);
}

TEST(OpdsFeedStreamTest, SinkCanStopEitherFormat) {
  std::string json = R"({"publications":[)";
  for (size_t i = 0; i < 5; ++i) json += (i ? "," : "") + jsonPublication(i);
  json += "]}";
  std::string atom = "<feed>";
  for (size_t i = 0; i < 5; ++i) {
    atom += "<entry><title>Book " + std::to_string(i) +
            "</title><link href='/b.epub' rel='http://opds-spec.org/acquisition' type='application/epub+zip'/></entry>";
  }
  atom += "</feed>";
  for (const auto& doc : {json, atom}) {
    StreamLog log;
    log.stopAfterBooks = 2;
    OpdsFeedParser parser(&streamEntry, &log);
    EXPECT_FALSE(feedChunks(parser, doc, 16));
    EXPECT_EQ(parser.getErrorReason(), OpdsParserError::SINK_ABORTED);
    EXPECT_EQ(log.books.size(), 2u);
  }
}

TEST(OpdsFeedStreamTest, AtomThroughTheFeedParserCountsEntriesSeen) {
  const std::string atom =
      "<feed><link rel='next' href='/p2'/>"
      "<entry><title>EPUB</title><link href='/a.epub' rel='http://opds-spec.org/acquisition' "
      "type='application/epub+zip'/></entry>"
      "<entry><title>PDF</title><link href='/a.pdf' rel='http://opds-spec.org/acquisition' "
      "type='application/pdf'/></entry></feed>";
  StreamLog log;
  OpdsFeedParser parser(&streamEntry, &log);
  ASSERT_TRUE(feedChunks(parser, atom, 5));
  EXPECT_EQ(log.books.size(), 1u);
  EXPECT_EQ(parser.getEntriesSeen(), 2u);
  EXPECT_EQ(parser.getNextPageUrl(), "/p2");
  EXPECT_FALSE(parser.hasGroupedPublications());
}
}  // namespace
