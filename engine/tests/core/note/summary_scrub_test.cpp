#include "core/note/summary_scrub.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

namespace clinicavt::note {
namespace {

using nlohmann::json;

json LoadFixture(const std::string& name) {
    std::ifstream in(std::string(CLINICAVT_FIXTURE_DIR) + "/" + name);
    if (!in.is_open()) throw std::runtime_error("missing fixture: " + name);
    return json::parse(in);
}

// Same fixture as the shell's identifier check
TEST(SummaryScrub, MatchesTheSharedFixture) {
    for (const auto& row : LoadFixture("summary-scrub.json")) {
        EXPECT_EQ(ScrubSummary(row["in"].get<std::string>()), row["out"].get<std::string>())
            << row["in"].get<std::string>();
    }
}

}  // namespace
}  // namespace clinicavt::note
