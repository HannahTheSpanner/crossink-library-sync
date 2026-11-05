#pragma once

// In-memory stand-in for HalStorage/HalFile with just the calls OpdsSyncQueue
// makes: open for read, write (truncate or append), exists, remove, rename.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>

using oflag_t = int;
inline constexpr oflag_t O_RDONLY = 0, O_WRONLY = 1, O_CREAT = 2, O_TRUNC = 4, O_APPEND = 8;

namespace fake_storage {
inline std::map<std::string, std::string> files;
inline bool failWrites = false;
inline bool failOpens = false;
inline std::string failReadPath;   // read() reports an error on this file
inline size_t readChunkLimit = 0;  // 0 = unlimited; otherwise reads at most this many bytes per call
}  // namespace fake_storage

class HalFile {
 public:
  HalFile() = default;
  HalFile(std::string path, const bool writing, const bool append) : path(std::move(path)), opened(true) {
    if (writing && !append) fake_storage::files[this->path].clear();
    writable = writing;
  }

  operator bool() const { return opened; }
  int read(void* buf, const size_t count) {
    if (!opened || writable) return -1;
    if (path == fake_storage::failReadPath) return -1;
    const std::string& data = fake_storage::files[path];
    if (position >= data.size()) return 0;
    size_t n = std::min(count, data.size() - position);
    if (fake_storage::readChunkLimit && n > fake_storage::readChunkLimit) n = fake_storage::readChunkLimit;
    memcpy(buf, data.data() + position, n);
    position += n;
    return static_cast<int>(n);
  }
  size_t write(const void* buf, const size_t count) {
    if (!opened || !writable || fake_storage::failWrites) return 0;
    fake_storage::files[path].append(static_cast<const char*>(buf), count);
    return count;
  }
  size_t write(const uint8_t b) { return write(&b, 1); }
  bool sync() { return opened; }
  bool close() {
    const bool was = opened;
    opened = false;
    return was;
  }

 private:
  std::string path;
  bool opened = false;
  bool writable = false;
  size_t position = 0;
};

struct HalStorageFake {
  HalFile open(const char* path, const oflag_t flags = O_RDONLY) {
    if (fake_storage::failOpens) return {};
    const bool writing = flags & O_WRONLY;
    if (!writing && !exists(path)) return {};
    return HalFile(path, writing, flags & O_APPEND);
  }
  bool exists(const char* path) { return fake_storage::files.count(path) > 0; }
  bool remove(const char* path) { return fake_storage::files.erase(path) > 0; }
  bool rename(const char* from, const char* to) {
    if (!exists(from) || exists(to)) return false;
    fake_storage::files[to] = std::move(fake_storage::files[from]);
    fake_storage::files.erase(from);
    return true;
  }
};

inline HalStorageFake Storage;
