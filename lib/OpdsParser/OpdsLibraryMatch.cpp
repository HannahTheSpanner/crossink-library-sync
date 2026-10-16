#include "OpdsLibraryMatch.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace opds_sync {

namespace {

constexpr uint64_t FNV_PRIME = 0x100000001b3ULL;

// Separate key domains so a file name can never equal a metadata or identity
// key by construction. Each prefix is hashed once and used as the seed.
constexpr std::string_view FILENAME_DOMAIN = "f:";
constexpr std::string_view METADATA_DOMAIN = "m:";
constexpr std::string_view IDENTITY_DOMAIN = "i:";

uint64_t domainHash(const std::string_view domain, const std::string_view text) {
  const uint64_t key = fnv1a64(text, fnv1a64(domain));
  return key == 0 ? 1 : key;  // 0 means "no key" throughout this file
}

// Base letters for U+00C0..U+00FF; nullptr marks a symbol (x, division sign).
// clang-format off
const char* const LATIN1_FOLD[64] = {
    "a",  "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",  //
    "d",  "n", "o", "o", "o", "o", "o",  nullptr, "o", "u", "u", "u", "u", "y", "th", "ss",
    "a",  "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",  //
    "d",  "n", "o", "o", "o", "o", "o",  nullptr, "o", "u", "u", "u", "u", "y", "th", "y",
};
// clang-format on

// Latin Extended-A (U+0100..U+017F) folded by contiguous runs.
struct FoldRun {
  uint16_t first;
  uint16_t last;
  const char* base;
};
// clang-format off
const FoldRun LATIN_EXT_A_FOLD[] = {
    {0x0100, 0x0105, "a"}, {0x0106, 0x010D, "c"}, {0x010E, 0x0111, "d"}, {0x0112, 0x011B, "e"},
    {0x011C, 0x0123, "g"}, {0x0124, 0x0127, "h"}, {0x0128, 0x0131, "i"}, {0x0132, 0x0133, "ij"},
    {0x0134, 0x0135, "j"}, {0x0136, 0x0138, "k"}, {0x0139, 0x0142, "l"}, {0x0143, 0x014B, "n"},
    {0x014C, 0x0151, "o"}, {0x0152, 0x0153, "oe"}, {0x0154, 0x0159, "r"}, {0x015A, 0x0161, "s"},
    {0x0162, 0x0167, "t"}, {0x0168, 0x0173, "u"}, {0x0174, 0x0175, "w"}, {0x0176, 0x0178, "y"},
    {0x0179, 0x017E, "z"}, {0x017F, 0x017F, "s"},
};
// clang-format on

// Decodes one UTF-8 sequence at `text[pos]`. Returns its length, or 0 for a
// malformed sequence (the caller then keeps the raw byte).
size_t decodeUtf8(const std::string_view text, const size_t pos, uint32_t& cp) {
  const auto lead = static_cast<unsigned char>(text[pos]);
  size_t len = 0;
  if (lead < 0x80) {
    cp = lead;
    return 1;
  } else if ((lead & 0xE0) == 0xC0) {
    cp = lead & 0x1F;
    len = 2;
  } else if ((lead & 0xF0) == 0xE0) {
    cp = lead & 0x0F;
    len = 3;
  } else if ((lead & 0xF8) == 0xF0) {
    cp = lead & 0x07;
    len = 4;
  } else {
    return 0;
  }
  if (pos + len > text.size()) return 0;
  for (size_t i = 1; i < len; ++i) {
    const auto next = static_cast<unsigned char>(text[pos + i]);
    if ((next & 0xC0) != 0x80) return 0;
    cp = (cp << 6) | (next & 0x3F);
  }
  return len;
}

// Folded replacement for a non-ASCII codepoint: a base-letter string, "" for
// a separator, or nullptr to keep the original bytes.
const char* foldCodepoint(const uint32_t cp) {
  if (cp >= 0x00C0 && cp <= 0x00FF) {
    const char* base = LATIN1_FOLD[cp - 0x00C0];
    return base ? base : "";
  }
  if (cp >= 0x0100 && cp <= 0x017F) {
    const auto* run = std::find_if(std::begin(LATIN_EXT_A_FOLD), std::end(LATIN_EXT_A_FOLD),
                                   [cp](const FoldRun& r) { return cp >= r.first && cp <= r.last; });
    if (run != std::end(LATIN_EXT_A_FOLD)) return run->base;
  }
  // Latin-1 punctuation and symbols (NBSP, guillemets, inverted marks),
  // General Punctuation (curly quotes, dashes, ellipsis, thin spaces) and
  // CJK punctuation (ideographic space and full stop, corner brackets).
  if ((cp >= 0x0080 && cp <= 0x00BF) || (cp >= 0x2000 && cp <= 0x206F) || (cp >= 0x3000 && cp <= 0x303F)) {
    return "";
  }
  // Fullwidth ASCII punctuation used in CJK titles.
  if ((cp >= 0xFF01 && cp <= 0xFF0F) || (cp >= 0xFF1A && cp <= 0xFF20) || (cp >= 0xFF3B && cp <= 0xFF40) ||
      (cp >= 0xFF5B && cp <= 0xFF65)) {
    return "";
  }
  return nullptr;
}

bool isAsciiAlnum(const char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

char asciiLower(const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string_view trimSpaces(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}

bool startsWithIgnoreCase(const std::string_view text, const std::string_view prefix) {
  if (text.size() < prefix.size()) return false;
  for (size_t i = 0; i < prefix.size(); ++i) {
    if (asciiLower(text[i]) != asciiLower(prefix[i])) return false;
  }
  return true;
}

int hexValue(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

std::string normalizeForComparison(const std::string_view text) {
  std::string out;
  out.reserve(text.size());
  bool pendingSpace = false;
  const auto appendWord = [&out, &pendingSpace](const char* bytes, const size_t len) {
    if (pendingSpace && !out.empty()) out.push_back(' ');
    pendingSpace = false;
    out.append(bytes, len);
  };

  size_t pos = 0;
  while (pos < text.size()) {
    const char c = text[pos];
    if (static_cast<unsigned char>(c) < 0x80) {
      if (isAsciiAlnum(c)) {
        const char lower = asciiLower(c);
        appendWord(&lower, 1);
      } else {
        pendingSpace = true;  // every ASCII symbol, space and '_' separates words
      }
      ++pos;
      continue;
    }

    uint32_t cp = 0;
    const size_t len = decodeUtf8(text, pos, cp);
    if (len == 0) {
      appendWord(&text[pos], 1);
      ++pos;
      continue;
    }
    const char* folded = foldCodepoint(cp);
    if (!folded) {
      appendWord(&text[pos], len);
    } else if (*folded == '\0') {
      pendingSpace = true;
    } else {
      appendWord(folded, strlen(folded));
    }
    pos += len;
  }
  return out;
}

uint64_t fnv1a64(const std::string_view text, uint64_t seed) {
  for (const char c : text) {
    seed ^= static_cast<unsigned char>(c);
    seed *= FNV_PRIME;
  }
  return seed;
}

std::string bookFilenameBase(const std::string& title, const std::string& author, const bool titleFirst) {
  if (author.empty()) return title;
  if (title.empty()) return author;
  if (titleFirst) return title + " - " + author;
  return author + " - " + title;
}

std::string authorLastFirst(const std::string_view author) {
  const std::string_view trimmed = trimSpaces(author);
  if (trimmed.empty() || trimmed.find(',') != std::string_view::npos) return std::string(trimmed);
  const size_t lastSpace = trimmed.find_last_of(' ');
  if (lastSpace == std::string_view::npos) return std::string(trimmed);
  const std::string_view surname = trimSpaces(trimmed.substr(lastSpace + 1));
  const std::string_view given = trimSpaces(trimmed.substr(0, lastSpace));
  std::string out;
  out.reserve(trimmed.size() + 2);
  out.append(surname);
  out.append(", ");
  out.append(given);
  return out;
}

std::string authorFirstLast(const std::string_view author) {
  const std::string_view trimmed = trimSpaces(author);
  const size_t comma = trimmed.find(',');
  if (comma == std::string_view::npos || trimmed.find(',', comma + 1) != std::string_view::npos) {
    return std::string(trimmed);
  }
  const std::string_view surname = trimSpaces(trimmed.substr(0, comma));
  const std::string_view given = trimSpaces(trimmed.substr(comma + 1));
  if (surname.empty() || given.empty()) return std::string(trimmed);
  std::string out;
  out.reserve(trimmed.size());
  out.append(given);
  out.push_back(' ');
  out.append(surname);
  return out;
}

std::string articleSortedTitle(const std::string_view title) {
  const std::string_view trimmed = trimSpaces(title);
  static constexpr std::string_view ARTICLES[] = {"The", "An", "A"};
  for (const auto article : ARTICLES) {
    if (trimmed.size() > article.size() + 1 && startsWithIgnoreCase(trimmed, article) &&
        trimmed[article.size()] == ' ') {
      const std::string_view rest = trimSpaces(trimmed.substr(article.size() + 1));
      if (rest.empty()) return {};
      std::string out;
      out.reserve(rest.size() + article.size() + 2);
      out.append(rest);
      out.append(", ");
      out.append(article);
      return out;
    }
  }
  return {};
}

std::string urlDecode(const std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '+') {
      out.push_back(' ');
    } else if (c == '%' && i + 2 < text.size()) {
      const int hi = hexValue(text[i + 1]);
      const int lo = hexValue(text[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
      } else {
        out.push_back(c);
      }
    } else {
      out.push_back(c);
    }
  }
  return out;
}

bool isEpubFilename(const std::string_view name) {
  constexpr std::string_view EXT = ".epub";
  if (name.size() <= EXT.size()) return false;
  const std::string_view tail = name.substr(name.size() - EXT.size());
  for (size_t i = 0; i < EXT.size(); ++i) {
    if (asciiLower(tail[i]) != EXT[i]) return false;
  }
  return true;
}

std::string_view filenameStem(std::string_view name) {
  const size_t slash = name.find_last_of("/\\");
  if (slash != std::string_view::npos) name.remove_prefix(slash + 1);
  const size_t dot = name.find_last_of('.');
  if (dot != std::string_view::npos && dot > 0) name = name.substr(0, dot);
  return name;
}

std::string epubStemFromHref(std::string_view href) {
  const size_t cut = href.find_first_of("?#");
  if (cut != std::string_view::npos) href = href.substr(0, cut);
  const size_t slash = href.find_last_of('/');
  if (slash != std::string_view::npos) href.remove_prefix(slash + 1);
  const std::string decoded = urlDecode(href);
  if (!isEpubFilename(decoded)) return {};
  return std::string(filenameStem(decoded));
}

uint64_t localFilenameKey(const std::string_view filename) {
  const std::string normalized = normalizeForComparison(filenameStem(filename));
  return normalized.empty() ? 0 : domainHash(FILENAME_DOMAIN, normalized);
}

uint64_t metadataKey(const std::string_view title, const std::string_view author) {
  const std::string normalizedTitle = normalizeForComparison(title);
  const std::string normalizedAuthor = normalizeForComparison(authorFirstLast(author));
  if (normalizedTitle.empty() || normalizedAuthor.empty()) return 0;
  // '\x1f' cannot occur in normalised text, so title/author boundaries stay unambiguous.
  const uint64_t titleHash = fnv1a64(normalizedTitle, fnv1a64(METADATA_DOMAIN));
  const uint64_t key = fnv1a64(normalizedAuthor, fnv1a64("\x1f", titleHash));
  return key == 0 ? 1 : key;
}

uint64_t remoteIdentityKey(const std::string_view atomId, const std::string_view absoluteHref) {
  const std::string_view id = trimSpaces(atomId);
  const std::string_view href = trimSpaces(absoluteHref);
  uint64_t key = 0;
  if (!id.empty()) {
    key = fnv1a64(id, fnv1a64("id:", fnv1a64(IDENTITY_DOMAIN)));
  } else if (!href.empty()) {
    key = fnv1a64(href, fnv1a64("url:", fnv1a64(IDENTITY_DOMAIN)));
  } else {
    return 0;
  }
  return key == 0 ? 1 : key;
}

void CandidateKeys::addKey(const uint64_t key) {
  if (key == 0 || count >= CAPACITY) return;
  for (size_t i = 0; i < count; ++i) {
    if (keys[i] == key) return;
  }
  keys[count++] = key;
}

void CandidateKeys::addStem(const std::string_view stem) {
  const std::string normalized = normalizeForComparison(stem);
  if (!normalized.empty()) addKey(domainHash(FILENAME_DOMAIN, normalized));
}

void CandidateKeys::addBookNames(const std::string& title, const std::string& author, const std::string_view href) {
  // CrossInk's own two naming formats.
  addStem(bookFilenameBase(title, author, false));
  addStem(bookFilenameBase(title, author, true));
  // library-sync's default "Last, First - Title".
  if (!author.empty() && !title.empty()) {
    addStem(authorLastFirst(author) + " - " + title);
  }
  // Calibre's article-sorted "Title, The - Author".
  const std::string sorted = articleSortedTitle(title);
  if (!sorted.empty()) addStem(bookFilenameBase(sorted, author, true));
  // Bare title, as many sideloaded files are named.
  addStem(title);
  // The server's own file name, when the link carries one.
  const std::string serverStem = epubStemFromHref(href);
  if (!serverStem.empty()) addStem(serverStem);
}

KeySet::~KeySet() { clear(); }

void KeySet::clear() {
  free(data);
  data = nullptr;
  count = 0;
  capacity = 0;
  sealed = true;
}

bool KeySet::add(const uint64_t key) {
  if (key == 0) return true;
  if (count == capacity) {
    const size_t nextCapacity = capacity == 0 ? 256 : capacity * 2;
    // realloc keeps the old block on failure, so a refused growth leaves the
    // set intact and the caller decides how to report it.
    auto* grown = static_cast<uint64_t*>(realloc(data, nextCapacity * sizeof(uint64_t)));
    if (!grown) return false;
    data = grown;
    capacity = nextCapacity;
  }
  data[count++] = key;
  sealed = false;
  return true;
}

void KeySet::seal() {
  if (sealed) return;
  std::sort(data, data + count);
  count = static_cast<size_t>(std::unique(data, data + count) - data);
  sealed = true;
}

bool KeySet::contains(const uint64_t key) const {
  if (key == 0 || count == 0 || !sealed) return false;
  return std::binary_search(data, data + count, key);
}

size_t KeySet::lowerBound(const uint64_t key) const {
  if (!sealed || count == 0) return count;
  return static_cast<size_t>(std::lower_bound(data, data + count, key) - data);
}

}  // namespace opds_sync
