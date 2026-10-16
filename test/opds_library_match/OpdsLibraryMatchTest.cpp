#include <OpdsLibraryMatch.h>
#include <gtest/gtest.h>

#include <string>

using namespace opds_sync;

namespace {

// True when a local file named `localName` would satisfy the catalogue book.
bool matches(const std::string& localName, const std::string& title, const std::string& author,
             const std::string& href = "/api/v1/opds/1/download") {
  CandidateKeys keys;
  keys.addBookNames(title, author, href);
  const uint64_t local = localFilenameKey(localName);
  for (size_t i = 0; i < keys.size(); ++i) {
    if (keys[i] == local) return true;
  }
  return false;
}

}  // namespace

TEST(OpdsLibraryMatchTest, NormalisationFoldsCaseAccentsAndPunctuation) {
  EXPECT_EQ(normalizeForComparison("Frank Herbert - Dune"), "frank herbert dune");
  EXPECT_EQ(normalizeForComparison("  Les Misérables  "), "les miserables");
  EXPECT_EQ(normalizeForComparison("ÉCOLE Ŝtraße"), "ecole strasse");
  EXPECT_EQ(normalizeForComparison("Ender\xE2\x80\x99s Game"), "ender s game");  // curly apostrophe
  EXPECT_EQ(normalizeForComparison("Ender's Game"), "ender s game");
  EXPECT_EQ(normalizeForComparison("Re_ Zero"), "re zero");  // sanitised ':'
  EXPECT_EQ(normalizeForComparison("Re: Zero"), "re zero");
  EXPECT_EQ(normalizeForComparison("Title, Vol. 1"), "title vol 1");
  const std::string emDash = "\xE2\x80\x94";
  EXPECT_EQ(normalizeForComparison("A" + emDash + "B"), "a b");
  EXPECT_EQ(normalizeForComparison("___"), "");
}

TEST(OpdsLibraryMatchTest, NonLatinScriptsAreKept) {
  // Japanese titles keep their characters; CJK punctuation separates.
  EXPECT_EQ(
      normalizeForComparison("\xE3\x83\x8E\xE3\x83\xAB\xE3\x82\xA6\xE3\x82\xA7\xE3\x82\xA4\xE3\x81\xAE\xE6\xA3\xAE"),
      "\xE3\x83\x8E\xE3\x83\xAB\xE3\x82\xA6\xE3\x82\xA7\xE3\x82\xA4\xE3\x81\xAE\xE6\xA3\xAE");
  EXPECT_EQ(normalizeForComparison("\xE3\x80\x8C\xE5\x90\x8D\xE3\x80\x8D"), "\xE5\x90\x8D");
  // Malformed UTF-8 is kept byte-for-byte rather than dropped.
  const std::string malformed = std::string("a") + '\xFF' + "b";
  EXPECT_EQ(normalizeForComparison(malformed), malformed);
}

TEST(OpdsLibraryMatchTest, AuthorAndTitleHelpers) {
  EXPECT_EQ(authorLastFirst("Frank Herbert"), "Herbert, Frank");
  EXPECT_EQ(authorLastFirst("Ursula K. Le Guin"), "Guin, Ursula K. Le");  // same rule as library-sync
  EXPECT_EQ(authorLastFirst("Herbert, Frank"), "Herbert, Frank");
  EXPECT_EQ(authorLastFirst("Homer"), "Homer");
  EXPECT_EQ(authorFirstLast("Herbert, Frank"), "Frank Herbert");
  EXPECT_EQ(authorFirstLast("Frank Herbert"), "Frank Herbert");
  EXPECT_EQ(articleSortedTitle("The Name of the Wind"), "Name of the Wind, The");
  EXPECT_EQ(articleSortedTitle("A Wizard of Earthsea"), "Wizard of Earthsea, A");
  EXPECT_EQ(articleSortedTitle("An Instance of the Fingerpost"), "Instance of the Fingerpost, An");
  EXPECT_EQ(articleSortedTitle("Anathem"), "");
  EXPECT_EQ(articleSortedTitle("Theory of Everything"), "");
  EXPECT_EQ(bookFilenameBase("Dune", "Frank Herbert", false), "Frank Herbert - Dune");
  EXPECT_EQ(bookFilenameBase("Dune", "Frank Herbert", true), "Dune - Frank Herbert");
  EXPECT_EQ(bookFilenameBase("Dune", "", true), "Dune");
  EXPECT_EQ(bookFilenameBase("", "Frank Herbert", false), "Frank Herbert");
}

TEST(OpdsLibraryMatchTest, HrefFilenames) {
  EXPECT_EQ(urlDecode("My%20Book+Two%2"), "My Book Two%2");
  EXPECT_EQ(epubStemFromHref("/books/My%20Book.epub?token=1#x"), "My Book");
  EXPECT_EQ(epubStemFromHref("/books/Upper.EPUB"), "Upper");
  EXPECT_EQ(epubStemFromHref("/api/v1/opds/90/download?fileId=691"), "");
  EXPECT_TRUE(isEpubFilename("a.epub"));
  EXPECT_TRUE(isEpubFilename("A.EPUB"));
  EXPECT_FALSE(isEpubFilename(".epub"));
  EXPECT_FALSE(isEpubFilename("a.epub.part"));
  EXPECT_EQ(filenameStem("/Books/Sub/Frank Herbert - Dune.epub"), "Frank Herbert - Dune");
  EXPECT_EQ(filenameStem("noext"), "noext");
}

TEST(OpdsLibraryMatchTest, RecognisesEveryNamingScheme) {
  // CrossInk's two formats.
  EXPECT_TRUE(matches("Frank Herbert - Dune.epub", "Dune", "Frank Herbert"));
  EXPECT_TRUE(matches("Dune - Frank Herbert.epub", "Dune", "Frank Herbert"));
  // library-sync default and Calibre article sort.
  EXPECT_TRUE(matches("Herbert, Frank - Dune.epub", "Dune", "Frank Herbert"));
  EXPECT_TRUE(matches("Name of the Wind, The - Patrick Rothfuss.epub", "The Name of the Wind", "Patrick Rothfuss"));
  // Bare title, different case and accents, sanitised punctuation.
  EXPECT_TRUE(matches("dune.EPUB", "Dune", "Frank Herbert"));
  EXPECT_TRUE(matches("Les Miserables.epub", "Les Misérables", "Victor Hugo"));
  EXPECT_TRUE(matches("Re_ Zero - Tappei Nagatsuki.epub", "Re: Zero", "Tappei Nagatsuki", "/x"));
  // Server file name from the link.
  EXPECT_TRUE(matches("dune_first_edition.epub", "Dune", "Frank Herbert", "/files/dune_first_edition.epub"));
  // Books without an author.
  EXPECT_TRUE(matches("Anonymous Tale.epub", "Anonymous Tale", ""));
}

TEST(OpdsLibraryMatchTest, DifferentBooksDoNotMatch) {
  EXPECT_FALSE(matches("Frank Herbert - Dune Messiah.epub", "Dune", "Frank Herbert"));
  EXPECT_FALSE(matches("Brian Herbert - Dune.epub", "Dune Messiah", "Frank Herbert"));
  EXPECT_FALSE(matches("Children of Dune.epub", "Dune", "Frank Herbert"));
}

TEST(OpdsLibraryMatchTest, CandidateKeysDeduplicate) {
  CandidateKeys keys;
  keys.addBookNames("Dune", "", "/x");  // every variant collapses to "dune"
  EXPECT_EQ(keys.size(), 1u);
  keys.addStem("DUNE!");
  EXPECT_EQ(keys.size(), 1u);
  keys.addStem("Dune (1965)");
  EXPECT_EQ(keys.size(), 2u);
}

TEST(OpdsLibraryMatchTest, MetadataKeysIgnoreAuthorOrder) {
  EXPECT_NE(metadataKey("Dune", "Frank Herbert"), 0u);
  EXPECT_EQ(metadataKey("Dune", "Frank Herbert"), metadataKey("DUNE", "Herbert, Frank"));
  EXPECT_NE(metadataKey("Dune", "Frank Herbert"), metadataKey("Dune", "Brian Herbert"));
  EXPECT_EQ(metadataKey("Dune", ""), 0u);
  EXPECT_EQ(metadataKey("", "Frank Herbert"), 0u);
  // Title/author boundary is unambiguous.
  EXPECT_NE(metadataKey("a b", "c"), metadataKey("a", "b c"));
  // Separate domain from file name keys.
  EXPECT_NE(metadataKey("dune", "frank herbert"), localFilenameKey("dune frank herbert.epub"));
}

TEST(OpdsLibraryMatchTest, IdentityKeysPreferAtomIds) {
  const uint64_t byId = remoteIdentityKey("urn:booklore:book:90", "https://h/api/v1/opds/90/download");
  EXPECT_EQ(byId, remoteIdentityKey("urn:booklore:book:90", "https://other/url"));
  EXPECT_NE(byId, remoteIdentityKey("urn:booklore:book:91", "https://h/api/v1/opds/90/download"));
  EXPECT_EQ(remoteIdentityKey("", "https://h/a.epub"), remoteIdentityKey("  ", "https://h/a.epub"));
  EXPECT_EQ(remoteIdentityKey("", ""), 0u);
}

TEST(OpdsLibraryMatchTest, KeySetSortsAndFinds) {
  KeySet set;
  EXPECT_FALSE(set.contains(5));
  for (uint64_t i = 1000; i > 0; --i) ASSERT_TRUE(set.add(i * 7));
  ASSERT_TRUE(set.add(7));        // duplicate
  ASSERT_TRUE(set.add(0));        // ignored
  EXPECT_FALSE(set.contains(7));  // not sealed yet
  set.seal();
  EXPECT_EQ(set.size(), 1000u);
  EXPECT_TRUE(set.contains(7));
  EXPECT_TRUE(set.contains(7000));
  EXPECT_FALSE(set.contains(8));
  EXPECT_FALSE(set.contains(0));
  EXPECT_EQ(set.lowerBound(7), 0u);
  EXPECT_EQ(set.lowerBound(8), 1u);
  EXPECT_EQ(set.at(set.lowerBound(13)), 14u);
  EXPECT_EQ(set.lowerBound(7001), set.size());
  set.clear();
  EXPECT_EQ(set.size(), 0u);
  EXPECT_FALSE(set.contains(7));
}
