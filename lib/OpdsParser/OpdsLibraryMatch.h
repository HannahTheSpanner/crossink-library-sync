#pragma once

// Matching rules for OPDS library sync: decide whether a catalogue book is
// already on the card without opening any book.
//
// Ported from the KOReader library-sync plugin (komadorirobin/library-sync.koplugin):
// a remote book counts as present when a local EPUB carries any of the names
// a downloader would plausibly have given it ("Author - Title", "Title -
// Author", article-sorted and "Last, First" variants, the bare title, or the
// server's own file name), compared in a folded form that ignores case,
// accents, punctuation and the characters filename sanitisers replace.
// On top of that, a book this device downloaded before is recognised by its
// stable OPDS identity, so renaming it on the server does not cause a
// duplicate download, and an indexed book is recognised by its EPUB
// title/author metadata, so oddly named files still count.
//
// Everything here is plain C++ with no Arduino or SD dependency, so it is
// covered by the native test suite.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace opds_sync {

// Case, accent and punctuation-insensitive comparison form. ASCII letters and
// digits are lowercased, Latin-1 and Latin Extended-A letters are folded to
// their base letters, punctuation and separators (including the '_' that
// filename sanitisers substitute) collapse to single spaces, and characters
// from other scripts are kept as-is.
std::string normalizeForComparison(std::string_view text);

// 64-bit FNV-1a. 64 bits keep the false-match odds negligible for libraries
// of many thousands of books while storing each key in 8 bytes.
uint64_t fnv1a64(std::string_view text, uint64_t seed = 0xcbf29ce484222325ULL);

// The base file name CrossInk's OPDS browser gives a download (before
// sanitising and the ".epub" suffix).
std::string bookFilenameBase(const std::string& title, const std::string& author, bool titleFirst);

// "First Middle Last" -> "Last, First Middle"; names that already contain a
// comma, and single words, are returned unchanged.
std::string authorLastFirst(std::string_view author);

// "Last, First" -> "First Last"; anything else is returned unchanged.
std::string authorFirstLast(std::string_view author);

// "The Name of the Wind" -> "Name of the Wind, The" (also "A" and "An");
// empty when the title does not start with an English article.
std::string articleSortedTitle(std::string_view title);

// Percent-decoding; '+' becomes a space.
std::string urlDecode(std::string_view text);

// True for names ending in ".epub" (any case).
bool isEpubFilename(std::string_view name);

// The name without its final extension and without any directory part.
std::string_view filenameStem(std::string_view name);

// Decoded file name stem of a link that names an EPUB
// ("/books/My%20Book.epub?x=1" -> "My Book"); empty otherwise.
std::string epubStemFromHref(std::string_view href);

// Key a local file contributes: its normalised name stem.
uint64_t localFilenameKey(std::string_view filename);

// Key for an EPUB's title and author metadata; 0 when either is empty.
uint64_t metadataKey(std::string_view title, std::string_view author);

// Stable identity of a catalogue entry: its Atom <id> when present, else
// its absolute acquisition URL. 0 when both are empty.
uint64_t remoteIdentityKey(std::string_view atomId, std::string_view absoluteHref);

// The distinct local-filename keys under which a catalogue book may already
// be on the card. Fixed capacity, so building one never allocates on the heap
// beyond the transient strings used while folding.
class CandidateKeys {
 public:
  static constexpr size_t CAPACITY = 10;

  // Adds every filename variant library-sync and CrossInk produce for this
  // book, plus the server file name when the link names one.
  void addBookNames(const std::string& title, const std::string& author, std::string_view href);
  // Adds one extra file name stem (for example the exact sanitised name the
  // downloader will write, which may have been shortened).
  void addStem(std::string_view stem);

  size_t size() const { return count; }
  uint64_t operator[](size_t index) const { return keys[index]; }

 private:
  void addKey(uint64_t key);

  uint64_t keys[CAPACITY] = {};
  size_t count = 0;
};

// Sorted set of 64-bit keys with fallible growth. Allocation failure is
// reported to the caller rather than aborting, because the C3 build has no
// exceptions and a large library must not crash the reader.
class KeySet {
 public:
  KeySet() = default;
  ~KeySet();
  KeySet(const KeySet&) = delete;
  KeySet& operator=(const KeySet&) = delete;

  // Append a key (0 is ignored). Returns false when memory runs out.
  bool add(uint64_t key);
  // Sort and drop duplicates. Must be called before contains().
  void seal();
  bool contains(uint64_t key) const;
  // Index of the first key >= `key` in a sealed set (size() when none).
  size_t lowerBound(uint64_t key) const;
  uint64_t at(size_t index) const { return data[index]; }
  size_t size() const { return count; }
  void clear();

 private:
  uint64_t* data = nullptr;
  size_t count = 0;
  size_t capacity = 0;
  bool sealed = true;
};

}  // namespace opds_sync
