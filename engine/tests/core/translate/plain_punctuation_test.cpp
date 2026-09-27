#include "core/translate/plain_punctuation.hpp"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace clinicavt::translate {
namespace {

TEST(PlainPunctuation, TypographyBecomesPlainAndNothingElseChanges) {
    const std::vector<std::pair<std::string, std::string>> rows = {
        {"Bell\xE2\x80\x99s palsy", "Bell's palsy"},
        {"\xE2\x80\x9CRest\xE2\x80\x9D, \xE2\x80\x98now\xE2\x80\x99", "\"Rest\", 'now'"},
        {"two\xE2\x80\x93six puffs \xE2\x80\x94 as needed", "two-six puffs - as needed"},
        {"400\xC2\xA0mg", "400 mg"},
        {"and so on\xE2\x80\xA6", "and so on..."},
        // Untouched: plain text and other UTF-8
        {"Take ibuprofen 400mg twice a day.", "Take ibuprofen 400mg twice a day."},
        {"caf\xC3\xA9 costs \xE2\x82\xAC five", "caf\xC3\xA9 costs \xE2\x82\xAC five"},
        {"", ""},
    };
    for (const auto& [text, plain] : rows) {
        EXPECT_EQ(PlainPunctuation(text), plain) << text;
    }
}

}  // namespace
}  // namespace clinicavt::translate
