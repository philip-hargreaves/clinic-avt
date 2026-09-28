#include "core/common/strings.hpp"

#include <gtest/gtest.h>

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace clinicavt::strings {
namespace {

TEST(Strings, WhitespaceCaseAndTokenHelpersSplitWhereTheirCallersNeed) {
    EXPECT_EQ(Lower("Dr Smith 5mg"), "dr smith 5mg");
    EXPECT_EQ(Lower("caf\xC3\xA9"), "caf\xC3\xA9");

    EXPECT_EQ(Trim("  a b \t\n"), "a b");
    EXPECT_EQ(Trim("   "), "");

    EXPECT_EQ(Words("  one two\tthree\n"), (std::vector<std::string_view>{"one", "two", "three"}));
    EXPECT_TRUE(Words(" \t").empty());
    EXPECT_EQ(WordCount("  one  two three "), 3);
    EXPECT_EQ(WordCount(""), 0);

    EXPECT_EQ(Squeeze("  a \t b\r\n c  "), "a b c");
    EXPECT_EQ(Squeeze(" \t "), "");

    const auto alpha = [](unsigned char c) { return std::isalpha(c) != 0; };
    EXPECT_EQ(LowerTokens("It's 5 O'Clock", alpha),
              (std::vector<std::string>{"it", "s", "o", "clock"}));
    EXPECT_EQ(
        LowerTokens("It's", [](unsigned char c) { return std::isalpha(c) != 0 || c == '\''; }),
        (std::vector<std::string>{"it's"}));
    EXPECT_TRUE(LowerTokens("--", alpha).empty());
}

TEST(Strings, UnixLinesTurnsEveryLineEndingIntoLf) {
    EXPECT_EQ(UnixLines("Heading\rLine one.\r\rNext"), "Heading\nLine one.\n\nNext");
    EXPECT_EQ(UnixLines("a\r\nb\nc\r"), "a\nb\nc\n");
    EXPECT_EQ(UnixLines("caf\xC3\xA9"), "caf\xC3\xA9");
    EXPECT_EQ(UnixLines(""), "");
}

TEST(Strings, EndsSentenceSetsClosersAside) {
    EXPECT_TRUE(EndsSentence("Done."));
    EXPECT_TRUE(EndsSentence("Really?\")  "));
    EXPECT_TRUE(EndsSentence("He said no.\xE2\x80\x9D"));  // closing curly quote
    EXPECT_FALSE(EndsSentence("and then"));
    EXPECT_FALSE(EndsSentence("e.g. this,"));
    EXPECT_FALSE(EndsSentence(""));
}

}  // namespace
}  // namespace clinicavt::strings
