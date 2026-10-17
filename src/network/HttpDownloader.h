#pragma once
#include <HalStorage.h>
#include <Stream.h>

#include <functional>
#include <string>
#include <string_view>
#include <utility>

/**
 * HTTP client utility for fetching content and downloading files.
 * Streams requests through the configured HTTP transport so large downloads
 * do not need to fit in RAM.
 */
class HttpDownloader {
 public:
  using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;
  using CancelCallback = std::function<bool()>;
  // Called with each body chunk as it arrives; return false to abort. Lets a
  // streaming parser consume the response without buffering the whole body.
  using DataCallback = std::function<bool(const uint8_t* data, size_t len)>;

  enum DownloadError {
    OK = 0,
    HTTP_ERROR,
    FILE_ERROR,
    ABORTED,
    INSUFFICIENT_SPACE,
  };

  enum class Transport {
    ESP_HTTP,
    WOLFSSL,
  };

  struct DownloadOptions {
    explicit DownloadOptions(bool preservePartial = false, bool resumePartial = false,
                             CancelCallback shouldCancel = nullptr, size_t bufferSize = 0,
                             Transport transport = Transport::ESP_HTTP)
        : preservePartial(preservePartial),
          resumePartial(resumePartial),
          shouldCancel(std::move(shouldCancel)),
          bufferSize(bufferSize),
          transport(transport) {}

    bool preservePartial;
    bool resumePartial;
    CancelCallback shouldCancel;
    size_t bufferSize;
    Transport transport;
    // Borrowed only for this synchronous request. Basic credentials are sent
    // only to this origin; empty keeps the request URL as the credential origin.
    std::string_view authorizationOrigin;
    // Download to "<destPath>.part" and rename it over destPath only once the
    // transfer succeeds, so a failed or interrupted download never replaces
    // (or deletes) an existing file and never leaves a truncated one behind
    // under the real name.
    bool stageAsPart = false;
    // Checks the finished file before it is accepted (and, with stageAsPart,
    // before it replaces destPath). Returning false fails the download and
    // removes the file. Catches bodies cut short without a Content-Length.
    bool (*validate)(const std::string& path) = nullptr;
    // Once the response length is known, fail with INSUFFICIENT_SPACE before
    // writing anything if the SD card cannot hold the file. The first check
    // can scan the whole FAT, so leave it off for small files.
    bool checkFreeSpace = false;
    // For a batch of downloads (library sync): a running estimate of free SD
    // space shared across them, so each download does not recount free
    // clusters. On a 32 GB FAT32 card that count reads ~15,000 sectors.
    // 0 means unknown, and the next download measures it. A successful
    // download lowers the estimate by the file size rounded up generously
    // (see reservedBytesForFile). When the estimate says a file may not fit,
    // the space is measured again before giving up. Used with checkFreeSpace.
    uint64_t* freeSpaceEstimate = nullptr;
    // When set, receives the final HTTP status: 0 when no response arrived
    // (connect, TLS, or transport failure), otherwise the status code, which
    // is 200 for a body that was cut short. Lets callers retry transient
    // failures without retrying a definitive answer such as 401 or 404.
    int* responseStatus = nullptr;
  };

  // True for failures worth retrying: no response, a body cut short, or a
  // server-side/temporary status (408, 429, 5xx). Client errors, a full SD
  // card, file errors and cancellation are definitive.
  static bool isRetryable(DownloadError result, int responseStatus);

  // Space a downloaded file is assumed to take when updating a free-space
  // estimate: its size rounded up to 128 KiB, the largest cluster size SD
  // cards normally use, plus 128 KiB for directory entries and the small
  // files a sync writes alongside it. Overestimating only means an early
  // re-measure near a full card; underestimating could start a download
  // that cannot fit.
  static constexpr uint64_t FREE_SPACE_ROUNDING = 128 * 1024;
  static constexpr uint64_t reservedBytesForFile(const uint64_t fileBytes) {
    return (fileBytes + FREE_SPACE_ROUNDING - 1) / FREE_SPACE_ROUNDING * FREE_SPACE_ROUNDING + FREE_SPACE_ROUNDING;
  }

  /**
   * Fetch text content from a URL with optional credentials.
   */
  static bool fetchUrl(const std::string& url, std::string& outContent, const std::string& username = "",
                       const std::string& password = "");

  static bool fetchUrl(const std::string& url, Stream& stream, const std::string& username = "",
                       const std::string& password = "");

  /**
   * Stream the response body to onData as it arrives, without buffering it.
   */
  static bool fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username = "",
                       const std::string& password = "");

  /**
   * Stream a URL with cancellation/progress support and a detailed result.
   */
  static DownloadError streamUrl(const std::string& url, const DataCallback& onData,
                                 ProgressCallback progress = nullptr, const std::string& username = "",
                                 const std::string& password = "", DownloadOptions options = DownloadOptions());

  /**
   * Download a file to the SD card with optional credentials.
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr, bool* cancelFlag = nullptr,
                                      const std::string& username = "", const std::string& password = "",
                                      DownloadOptions options = DownloadOptions());
};
