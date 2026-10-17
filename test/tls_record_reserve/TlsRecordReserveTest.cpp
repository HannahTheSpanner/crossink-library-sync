#include <gtest/gtest.h>
#include <network/TlsRecordReserve.h>

#include <cstdlib>
#include <set>

namespace {

// Backend that records live allocations and can be told to fail.
struct FakeHeap {
  static std::set<void*>& live() {
    static std::set<void*> s;
    return s;
  }
  static bool& failNext() {
    static bool f = false;
    return f;
  }
  static void* allocate(size_t size) {
    if (failNext()) {
      failNext() = false;
      return nullptr;
    }
    void* p = malloc(size);
    live().insert(p);
    return p;
  }
  static void deallocate(void* p) {
    if (!p) return;
    ASSERT_EQ(live().erase(p), 1u) << "free of a pointer the heap does not own";
    free(p);
  }
  static void* reallocate(void* p, size_t size) {
    if (p) live().erase(p);
    void* q = realloc(p, size);
    live().insert(q);
    return q;
  }
};

class TlsRecordReserveTest : public ::testing::Test {
 protected:
  void SetUp() override {
    TlsRecordReserve::testing::reset();
    FakeHeap::live().clear();
    FakeHeap::failNext() = false;
    TlsRecordReserve::testing::setBackend({&FakeHeap::allocate, &FakeHeap::deallocate, &FakeHeap::reallocate});
  }
  void TearDown() override {
    TlsRecordReserve::release();
    EXPECT_TRUE(FakeHeap::live().empty()) << "leaked allocations";
    TlsRecordReserve::testing::reset();
  }
};

constexpr unsigned FULL_RECORD = 16384 + 17 + 5;  // what a 16 KB TLS 1.3 record needs

}  // namespace

TEST_F(TlsRecordReserveTest, AcquireAllocatesOnceAndIsIdempotent) {
  EXPECT_FALSE(TlsRecordReserve::held());
  ASSERT_TRUE(TlsRecordReserve::acquire());
  EXPECT_TRUE(TlsRecordReserve::held());
  EXPECT_EQ(FakeHeap::live().size(), 1u);
  ASSERT_TRUE(TlsRecordReserve::acquire());
  EXPECT_EQ(FakeHeap::live().size(), 1u);
}

TEST_F(TlsRecordReserveTest, AcquireFailureLeavesNoReserve) {
  FakeHeap::failNext() = true;
  EXPECT_FALSE(TlsRecordReserve::acquire());
  EXPECT_FALSE(TlsRecordReserve::held());
  unsigned capacity = 0;
  EXPECT_EQ(FreeInkTlsRecordReserveTake(100, &capacity), nullptr);
}

TEST_F(TlsRecordReserveTest, TakeLendsFullCapacityOnce) {
  ASSERT_TRUE(TlsRecordReserve::acquire());
  unsigned capacity = 0;
  void* block = FreeInkTlsRecordReserveTake(120, &capacity);
  ASSERT_NE(block, nullptr);
  // The full capacity is what lets wolfSSL hold a later 16 KB record without
  // growing (and so without a second large allocation).
  EXPECT_EQ(capacity, TlsRecordReserve::RESERVE_BYTES);
  EXPECT_GE(capacity, FULL_RECORD);
  EXPECT_TRUE(TlsRecordReserve::testing::lent());
  unsigned other = 0;
  EXPECT_EQ(FreeInkTlsRecordReserveTake(120, &other), nullptr) << "only one connection may borrow the block";
  TlsRecordReserve::testing::hookFree(block);
  EXPECT_FALSE(TlsRecordReserve::testing::lent());
  EXPECT_TRUE(TlsRecordReserve::held()) << "returning the block must not free it";
  EXPECT_EQ(FreeInkTlsRecordReserveTake(FULL_RECORD, &capacity), block) << "the next connection reuses it";
  TlsRecordReserve::testing::hookFree(block);
}

TEST_F(TlsRecordReserveTest, OversizedRequestsFallBackToTheHeap) {
  ASSERT_TRUE(TlsRecordReserve::acquire());
  unsigned capacity = 0;
  EXPECT_EQ(FreeInkTlsRecordReserveTake(TlsRecordReserve::RESERVE_BYTES + 1, &capacity), nullptr);
  EXPECT_FALSE(TlsRecordReserve::testing::lent());
}

TEST_F(TlsRecordReserveTest, HooksForwardOtherAllocations) {
  ASSERT_TRUE(TlsRecordReserve::acquire());
  void* p = TlsRecordReserve::testing::hookMalloc(64);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(FakeHeap::live().size(), 2u);
  p = TlsRecordReserve::testing::hookRealloc(p, 128);
  ASSERT_NE(p, nullptr);
  TlsRecordReserve::testing::hookFree(p);
  TlsRecordReserve::testing::hookFree(nullptr);
  EXPECT_EQ(FakeHeap::live().size(), 1u);
}

TEST_F(TlsRecordReserveTest, ReleaseWhileLentWaitsForTheConnection) {
  ASSERT_TRUE(TlsRecordReserve::acquire());
  unsigned capacity = 0;
  void* block = FreeInkTlsRecordReserveTake(120, &capacity);
  ASSERT_NE(block, nullptr);
  TlsRecordReserve::release();
  EXPECT_TRUE(TlsRecordReserve::held()) << "the connection is still using it";
  EXPECT_EQ(FakeHeap::live().size(), 1u);
  TlsRecordReserve::testing::hookFree(block);
  EXPECT_FALSE(TlsRecordReserve::held());
  EXPECT_TRUE(FakeHeap::live().empty());
}

TEST_F(TlsRecordReserveTest, NotLentWhileReleasePending) {
  ASSERT_TRUE(TlsRecordReserve::acquire());
  unsigned capacity = 0;
  void* block = FreeInkTlsRecordReserveTake(120, &capacity);
  TlsRecordReserve::release();
  TlsRecordReserve::testing::hookFree(block);
  EXPECT_EQ(FreeInkTlsRecordReserveTake(120, &capacity), nullptr);
}

TEST_F(TlsRecordReserveTest, AcquireCancelsAPendingRelease) {
  ASSERT_TRUE(TlsRecordReserve::acquire());
  unsigned capacity = 0;
  void* block = FreeInkTlsRecordReserveTake(120, &capacity);
  TlsRecordReserve::release();
  ASSERT_TRUE(TlsRecordReserve::acquire());
  TlsRecordReserve::testing::hookFree(block);
  EXPECT_TRUE(TlsRecordReserve::held());
  EXPECT_EQ(FreeInkTlsRecordReserveTake(120, &capacity), block);
  TlsRecordReserve::testing::hookFree(block);
}

TEST_F(TlsRecordReserveTest, SmallRecordsReturnTheBlock) {
  ASSERT_TRUE(TlsRecordReserve::acquire());
  FreeInkTlsRecordReserveSmallRecords(2048);
  EXPECT_FALSE(TlsRecordReserve::held());
  EXPECT_TRUE(FakeHeap::live().empty());
}

TEST_F(TlsRecordReserveTest, SmallRecordsDuringAHandshakeFreeAtClose) {
  ASSERT_TRUE(TlsRecordReserve::acquire());
  unsigned capacity = 0;
  // ServerHello arrives before the server's max_fragment_length answer.
  void* block = FreeInkTlsRecordReserveTake(120, &capacity);
  FreeInkTlsRecordReserveSmallRecords(2048);
  EXPECT_TRUE(TlsRecordReserve::held());
  TlsRecordReserve::testing::hookFree(block);
  EXPECT_FALSE(TlsRecordReserve::held());
}

TEST_F(TlsRecordReserveTest, ReallocOfTheBlockStaysCorrect) {
  ASSERT_TRUE(TlsRecordReserve::acquire());
  unsigned capacity = 0;
  void* block = FreeInkTlsRecordReserveTake(120, &capacity);
  EXPECT_EQ(TlsRecordReserve::testing::hookRealloc(block, 1000), block);
  void* moved = TlsRecordReserve::testing::hookRealloc(block, TlsRecordReserve::RESERVE_BYTES + 100);
  ASSERT_NE(moved, nullptr);
  EXPECT_NE(moved, block);
  EXPECT_FALSE(TlsRecordReserve::testing::lent());
  TlsRecordReserve::testing::hookFree(moved);
}
