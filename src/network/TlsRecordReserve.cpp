#include "TlsRecordReserve.h"

#include <Logging.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(FREEINK_NET_WOLFSSL)
#include <wolfssl/wolfcrypt/memory.h>
#include <wolfssl/wolfcrypt/settings.h>

// scripts/patch_wolfssl.py must have run: it defines these for the wolfSSL
// build and patches its input buffer. Fail the build rather than ship firmware
// that silently lacks the fix.
#if !defined(FREEINK_TLS_RECORD_RESERVE) || !defined(HAVE_MAX_FRAGMENT)
#error "wolfSSL was not patched by scripts/patch_wolfssl.py (TLS record reserve)"
#endif
extern "C" const int FreeInkTlsRecordReservePatched;
#endif

namespace {

using Backend = TlsRecordReserve::testing::Backend;

void* stdAllocate(size_t size) { return malloc(size); }
void stdDeallocate(void* ptr) { free(ptr); }
void* stdReallocate(void* ptr, size_t size) { return realloc(ptr, size); }

Backend backend{&stdAllocate, &stdDeallocate, &stdReallocate};

// All state is touched only from the task that runs wolfSSL (the main loop);
// wolfSSL is built SINGLE_THREADED.
uint8_t* block = nullptr;
bool blockLent = false;
bool releasePending = false;
bool hooksInstalled = false;
bool loggedSmallRecords = false;

bool owns(const void* ptr) { return block && ptr >= block && ptr < block + TlsRecordReserve::RESERVE_BYTES; }

void freeBlock() {
  backend.deallocate(block);
  block = nullptr;
  blockLent = false;
  releasePending = false;
}

bool installHooks() {
  if (hooksInstalled) return true;
#if defined(FREEINK_NET_WOLFSSL)
  // Every wolfSSL allocation now passes through the hooks; anything that is not
  // the block is forwarded to the heap unchanged, so pointers wolfSSL obtained
  // before this call are still freed correctly.
  if (wolfSSL_SetAllocators(&TlsRecordReserve::testing::hookMalloc, &TlsRecordReserve::testing::hookFree,
                            &TlsRecordReserve::testing::hookRealloc) != 0) {
    LOG_ERR("TLS", "Could not install wolfSSL allocator hooks");
    return false;
  }
#endif
  hooksInstalled = true;
  return true;
}

}  // namespace

namespace TlsRecordReserve {

bool acquire() {
#if defined(BOARD_HAS_PSRAM)
  // PSRAM serves large allocations; the internal heap never needs to provide
  // a record-sized block.
  return true;
#else
  if (block) {
    releasePending = false;
    return true;
  }
#if defined(FREEINK_NET_WOLFSSL)
  // Link-time proof that wolfSSL's input buffer can borrow the block.
  if (!FreeInkTlsRecordReservePatched) return false;
#endif
  if (!installHooks()) return false;
  // Fallible allocation: the caller continues without a reserve (and with the
  // previous behaviour) if even this early block is unavailable.
  block = static_cast<uint8_t*>(backend.allocate(RESERVE_BYTES));
  if (!block) {
    LOG_ERR("TLS", "Could not reserve a %u-byte TLS record buffer", static_cast<unsigned>(RESERVE_BYTES));
    return false;
  }
  loggedSmallRecords = false;
  LOG_INF("TLS", "Reserved %u-byte TLS record buffer", static_cast<unsigned>(RESERVE_BYTES));
  return true;
#endif
}

void release() {
  if (!block) return;
  if (blockLent) {
    releasePending = true;
    return;
  }
  freeBlock();
  LOG_INF("TLS", "Released TLS record buffer");
}

bool held() { return block != nullptr; }

namespace testing {

void setBackend(const Backend& replacement) { backend = replacement; }

void reset() {
  block = nullptr;
  blockLent = false;
  releasePending = false;
  hooksInstalled = false;
  loggedSmallRecords = false;
  backend = Backend{&stdAllocate, &stdDeallocate, &stdReallocate};
}

void* hookMalloc(const size_t size) { return backend.allocate(size); }

void hookFree(void* ptr) {
  if (!ptr) return;
  if (owns(ptr)) {
    blockLent = false;
    if (releasePending) {
      freeBlock();
      LOG_INF("TLS", "Released TLS record buffer");
    }
    return;
  }
  backend.deallocate(ptr);
}

void* hookRealloc(void* ptr, const size_t size) {
  if (!owns(ptr)) return backend.reallocate(ptr, size);
  // wolfSSL never reallocates its input buffer; stay correct if it ever does.
  if (size <= RESERVE_BYTES) return ptr;
  void* moved = backend.allocate(size);
  if (!moved) return nullptr;
  memcpy(moved, ptr, RESERVE_BYTES);
  hookFree(ptr);
  return moved;
}

bool lent() { return blockLent; }

}  // namespace testing
}  // namespace TlsRecordReserve

extern "C" void* FreeInkTlsRecordReserveTake(const unsigned int want, unsigned int* capacity) {
  if (!block || blockLent || want > TlsRecordReserve::RESERVE_BYTES || releasePending) return nullptr;
  blockLent = true;
  if (capacity) *capacity = static_cast<unsigned int>(TlsRecordReserve::RESERVE_BYTES);
  return block;
}

extern "C" void FreeInkTlsRecordReserveSmallRecords([[maybe_unused]] const unsigned int maxFragment) {
  if (!loggedSmallRecords) {
    loggedSmallRecords = true;
    LOG_INF("TLS", "Server accepted %u-byte TLS records", maxFragment);
  }
  // A connection that already borrowed the block keeps it until it closes;
  // later connections to this server allocate small buffers normally.
  TlsRecordReserve::release();
}
