#include <HalStorage.h>
#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "OpdsSyncQueue.h"

namespace {

constexpr char QUEUE[] = "/.crosspoint/opds_sync_queue.tsv";
constexpr char PAGE[] = "/.crosspoint/opds_sync_page.tsv";
constexpr char LEDGER[] = "/.crosspoint/opds_sync_abc.txt";

class OpdsSyncQueueTest : public ::testing::Test {
 protected:
  void SetUp() override {
    fake_storage::files.clear();
    fake_storage::failWrites = false;
    fake_storage::failOpens = false;
    fake_storage::failReadPath.clear();
    fake_storage::readChunkLimit = 0;
    queue = std::make_unique<OpdsSyncQueue>(QUEUE, PAGE, LEDGER, buffer);
  }

  char buffer[OpdsSyncQueue::RECORD_BUFFER_SIZE];
  std::unique_ptr<OpdsSyncQueue> queue;
};

std::string hexKey(const uint64_t key) {
  char text[OpdsSyncQueue::KEY_TEXT_SIZE];
  OpdsSyncQueue::formatKey(key, text);
  return text;
}

TEST_F(OpdsSyncQueueTest, KeysRoundTripThroughSixteenHexDigits) {
  EXPECT_EQ(hexKey(0), "0000000000000000");
  EXPECT_EQ(hexKey(0x0123456789abcdefULL), "0123456789abcdef");
  uint64_t parsed = 0;
  EXPECT_TRUE(OpdsSyncQueue::parseKey("0123456789ABCDEF", parsed));
  EXPECT_EQ(parsed, 0x0123456789abcdefULL);
  EXPECT_FALSE(OpdsSyncQueue::parseKey("0123456789abcde", parsed));    // short
  EXPECT_FALSE(OpdsSyncQueue::parseKey("0123456789abcdeg", parsed));   // not hex
  EXPECT_FALSE(OpdsSyncQueue::parseKey("0123456789abcdef0", parsed));  // long
}

TEST_F(OpdsSyncQueueTest, PagesCommitOnlyWhenAskedAndRecordsReadBackInOrder) {
  queue->reset();
  ASSERT_TRUE(queue->beginPage());
  ASSERT_TRUE(queue->appendPageRecord(1, false, "http://s/1.epub", "One\tTab", "Author\nLine"));
  ASSERT_TRUE(queue->appendPageRecord(2, true, "http://s/2.json", "Two", ""));
  ASSERT_TRUE(queue->endPage());
  ASSERT_TRUE(queue->commitPage(2));
  // A page that fails partway is simply not committed.
  ASSERT_TRUE(queue->beginPage());
  ASSERT_TRUE(queue->appendPageRecord(3, false, "http://s/3.epub", "Dropped", "X"));
  ASSERT_TRUE(queue->endPage());
  // Then the next page replaces it.
  ASSERT_TRUE(queue->beginPage());
  ASSERT_TRUE(queue->appendPageRecord(4, false, "http://s/4.epub", "Four", "Y"));
  ASSERT_TRUE(queue->endPage());
  ASSERT_TRUE(queue->commitPage(1));
  // A page with nothing missing opens no files.
  ASSERT_TRUE(queue->commitPage(0));

  ASSERT_TRUE(queue->openForReading());
  OpdsSyncQueue::Record record;
  ASSERT_TRUE(queue->next(record));
  EXPECT_EQ(record.identity, hexKey(1));
  EXPECT_FALSE(record.publicationDocument);
  EXPECT_EQ(record.url, "http://s/1.epub");
  EXPECT_EQ(record.title, "One Tab");  // delimiters are replaced by spaces
  EXPECT_EQ(record.author, "Author Line");
  ASSERT_TRUE(queue->next(record));
  EXPECT_EQ(record.identity, hexKey(2));
  EXPECT_TRUE(record.publicationDocument);
  EXPECT_EQ(record.title, "Two");
  EXPECT_TRUE(record.author.empty());
  ASSERT_TRUE(queue->next(record));
  EXPECT_EQ(record.identity, hexKey(4));
  EXPECT_FALSE(queue->next(record));
  EXPECT_EQ(queue->skippedRecords(), 0u);
  queue->closeReader();

  queue->removeWorkFiles();
  EXPECT_FALSE(Storage.exists(QUEUE));
  EXPECT_FALSE(Storage.exists(PAGE));
}

TEST_F(OpdsSyncQueueTest, DamagedRecordsAreSkippedAndCounted) {
  fake_storage::files[QUEUE] = hexKey(1) + "\te\thttp://s/1.epub\tOne\tA\n" + "garbage line\n" +  // too few fields
                               "notahexkey000000\te\thttp://s/x.epub\tBad\tB\n" +  // key length ok, parse not checked
                               hexKey(3) + "\tz\t\tEmpty url\tC\n" +               // empty URL
                               hexKey(4) + "\tp\thttp://s/4.json\tFour\n";         // no author field is fine
  ASSERT_TRUE(queue->openForReading());
  OpdsSyncQueue::Record record;
  ASSERT_TRUE(queue->next(record));
  EXPECT_EQ(record.identity, hexKey(1));
  ASSERT_TRUE(queue->next(record));  // the 16-character key is accepted as text
  EXPECT_EQ(record.identity, "notahexkey000000");
  ASSERT_TRUE(queue->next(record));
  EXPECT_EQ(record.identity, hexKey(4));
  EXPECT_TRUE(record.publicationDocument);
  EXPECT_TRUE(record.author.empty());
  EXPECT_FALSE(queue->next(record));
  EXPECT_EQ(queue->skippedRecords(), 2u);
}

TEST_F(OpdsSyncQueueTest, OverlongRecordsAreSkippedWholeAcrossSmallReads) {
  const std::string huge(OpdsSyncQueue::RECORD_BUFFER_SIZE * 2, 'u');
  fake_storage::files[QUEUE] =
      hexKey(1) + "\te\thttp://s/" + huge + ".epub\tLong\tA\n" + hexKey(2) + "\te\thttp://s/2.epub\tTwo\tB\n";
  fake_storage::readChunkLimit = 100;  // the file arrives in small pieces
  ASSERT_TRUE(queue->openForReading());
  OpdsSyncQueue::Record record;
  ASSERT_TRUE(queue->next(record));
  EXPECT_EQ(record.identity, hexKey(2));
  EXPECT_FALSE(queue->next(record));
}

TEST_F(OpdsSyncQueueTest, LedgerKeepsBooksStillOnTheCardAndDropsTheRest) {
  fake_storage::files["/books/kept.epub"] = "epub";
  fake_storage::files[LEDGER] =
      hexKey(10) + "\t/books/kept.epub\n" + hexKey(11) + "\t/books/gone.epub\n" + "broken\n" + hexKey(12) + "\t\n";
  opds_sync::KeySet present;
  size_t stale = 99;
  ASSERT_TRUE(queue->loadLedger(present, stale));
  present.seal();
  EXPECT_EQ(stale, 3u);
  EXPECT_EQ(present.size(), 1u);
  EXPECT_TRUE(present.contains(10));
  EXPECT_FALSE(present.contains(11));
  // The ledger was rewritten with only the live entry.
  EXPECT_EQ(fake_storage::files[LEDGER], hexKey(10) + "\t/books/kept.epub\n");
  EXPECT_FALSE(Storage.exists((std::string(LEDGER) + ".tmp").c_str()));

  queue->appendLedger(hexKey(13), "/books/new\tbook.epub");
  EXPECT_EQ(fake_storage::files[LEDGER], hexKey(10) + "\t/books/kept.epub\n" + hexKey(13) + "\t/books/new book.epub\n");
}

TEST_F(OpdsSyncQueueTest, MissingLedgerIsNotAnError) {
  opds_sync::KeySet present;
  size_t stale = 99;
  EXPECT_TRUE(queue->loadLedger(present, stale));
  EXPECT_EQ(stale, 0u);
  EXPECT_EQ(present.size(), 0u);
}

TEST_F(OpdsSyncQueueTest, StorageFailuresAreReported) {
  fake_storage::failOpens = true;
  EXPECT_FALSE(queue->beginPage());
  EXPECT_FALSE(queue->openForReading());
  fake_storage::failOpens = false;

  ASSERT_TRUE(queue->beginPage());
  fake_storage::failWrites = true;
  EXPECT_FALSE(queue->appendPageRecord(1, false, "http://s/1.epub", "One", "A"));
  fake_storage::failWrites = false;
  ASSERT_TRUE(queue->endPage());

  fake_storage::failReadPath = PAGE;
  EXPECT_FALSE(queue->commitPage(1));
}

}  // namespace
