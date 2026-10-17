#pragma once

#include <cstddef>

// One TLS record buffer, set aside before the heap fragments.
//
// wolfSSL must hold a complete incoming TLS record before it can decrypt it,
// and HTTPS servers commonly send bulk data in 16 KB records. On ESP32-C3
// boards (X3/X4, no PSRAM) the ~16.7 KB contiguous block that needs is usually
// gone once WiFi is up and an activity has run: free heap is ~13 KB with a
// largest block of 2-6 KB. Small OPDS pages arrive in small records and load;
// book lists and EPUBs arrive in full records and failed with wolfSSL
// MEMORY_E (-125), surfacing as "Failed to fetch feed" / "Download failed".
//
// acquire() takes the block while a large one still exists (OPDS start,
// before WiFi). wolfSSL's input-buffer growth (patched by
// scripts/patch_wolfssl.py) borrows it at full capacity, so a connection never
// needs a second large allocation, and the allocator hooks take it back when
// the connection frees its buffer. When a server accepts the 2 KB
// max_fragment_length request, no record will need the block, so it goes back
// to the heap.
namespace TlsRecordReserve {

// The largest TLS 1.3 ciphertext record (2^14 + 256), its 5-byte header, and
// slack for any partial next record wolfSSL carries over.
constexpr size_t RESERVE_BYTES = 16384 + 256 + 5 + 64;

// Reserve the block. Returns true once reserved, or on PSRAM boards, where
// large allocations are served from PSRAM and no reserve is needed.
// Idempotent; returns false only if the block could not be allocated.
bool acquire();

// Return the block to the heap now, or as soon as the connection borrowing it
// frees its input buffer.
void release();

// True while the block exists (lent or not).
bool held();

namespace testing {
// Underlying heap used for every allocation the hooks forward, and for the
// block itself. Host tests substitute a constrained heap; firmware uses the
// default (malloc/free/realloc).
struct Backend {
  void* (*allocate)(size_t size);
  void (*deallocate)(void* ptr);
  void* (*reallocate)(void* ptr, size_t size);
};
void setBackend(const Backend& backend);
void reset();
// The wolfSSL allocator hooks, exposed so tests can drive them directly.
void* hookMalloc(size_t size);
void hookFree(void* ptr);
void* hookRealloc(void* ptr, size_t size);
bool lent();
}  // namespace testing

}  // namespace TlsRecordReserve

// Called from the patched wolfSSL sources, which are C.
extern "C" {
// Lend the block to a wolfSSL input buffer that needs `want` bytes. Returns
// null when there is no free block or it is too small; on success *capacity
// is the usable size, which wolfSSL records so the buffer never grows again.
void* FreeInkTlsRecordReserveTake(unsigned int want, unsigned int* capacity);
// The server accepted max_fragment_length: its records are at most
// `maxFragment` bytes, so the block is not needed.
void FreeInkTlsRecordReserveSmallRecords(unsigned int maxFragment);
}
