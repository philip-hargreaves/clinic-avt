#include "adapters/guidance/ingest_host.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

#include "tiny_pdf.hpp"

namespace clinicavt::guidance {
namespace {

std::vector<std::uint8_t> Bytes(const std::string& s) {
    return {s.begin(), s.end()};
}

IngestHost Fake(HostLimits limits = {}) {
    return IngestHost(CLINICAVT_FAKE_INGEST_HOST, limits);
}

std::string ReasonOf(const IngestHost& host, const std::string& input) {
    try {
        host.Extract(Bytes(input));
    } catch (const HostError& e) {
        return e.Reason();
    }
    return "";
}

TEST(IngestHost, PagesComeBackAsFractionsAndALargeInputArrivesWhole) {
    const auto pages = Fake().Extract(Bytes("%PDF canned"));
    ASSERT_EQ(pages.size(), 2u);
    EXPECT_FLOAT_EQ(pages[0].width, 595);
    ASSERT_EQ(pages[0].lines.size(), 2u);
    EXPECT_EQ(pages[0].lines[0].text, "1.1 Offer allopurinol after a first attack.");
    EXPECT_NEAR(pages[0].lines[0].box.left, 72.0F / 595, 1e-4);
    EXPECT_NEAR(pages[0].lines[0].box.bottom, 84.0F / 842, 1e-4);
    EXPECT_TRUE(pages[1].lines.empty());

    // Far past a pipe's buffer, so writing it all before reading would deadlock
    std::string input = "FAKE echo";
    input.resize(5 << 20, 'p');
    const auto echoed = Fake().Extract(Bytes(input));
    ASSERT_EQ(echoed.size(), 1u);
    ASSERT_EQ(echoed[0].lines.size(), 1u);
    EXPECT_EQ(echoed[0].lines[0].text, std::to_string(5 << 20) + " bytes");
}

TEST(IngestHost, EveryWayTheHostFailsBecomesANamedRefusal) {
    HostLimits stall;
    stall.timeout = std::chrono::milliseconds(500);
    HostLimits capped;
    capped.output_cap = 1 << 20;
    struct Case {
        const char* input;
        HostLimits limits;
        const char* reason;
    };
    const Case cases[] = {
        {"FAKE exit 2", {}, "cannotOpen"},    {"FAKE exit 3", {}, "password"},
        {"FAKE exit 4", {}, "outputBound"},   {"FAKE exit 7", {}, "crashed"},
        {"FAKE crash", {}, "crashed"},        {"FAKE garbage", {}, "badOutput"},
        {"FAKE partial", {}, "badOutput"},    {"FAKE sleep", stall, "timeout"},
        {"FAKE huge", capped, "outputBound"},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.input);
        const auto start = std::chrono::steady_clock::now();
        EXPECT_EQ(ReasonOf(Fake(c.limits), c.input), c.reason);
        EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(10))
            << "a stalled host is killed, not waited on";
    }
}

TEST(IngestHost, TheRealHostReadsAndDrawsAPdf) {
    const IngestHost host(CLINICAVT_INGEST_HOST);
    const auto pages = host.Extract(fixture::TinyPdf(
        {"Offer allopurinol.", "Check urate six weeks later.", "   Offer allopurinol.        "}));
    ASSERT_EQ(pages.size(), 1u);
    EXPECT_NEAR(pages[0].width, 595, 0.5);
    ASSERT_EQ(pages[0].lines.size(), 3u);
    const auto& lines = pages[0].lines;
    EXPECT_EQ(lines[0].text, "Offer allopurinol.");
    EXPECT_EQ(lines[1].text, "Check urate six weeks later.");
    EXPECT_LT(lines[0].box.top, lines[1].box.top);
    EXPECT_NEAR(lines[0].box.left, 72.0F / 595, 0.01);
    // Spaces around a line shape neither its box nor its text
    EXPECT_EQ(lines[2].text, "Offer allopurinol.");
    EXPECT_GT(lines[2].box.left, lines[0].box.left);
    EXPECT_NEAR(lines[2].box.right - lines[2].box.left, lines[0].box.right - lines[0].box.left,
                1e-3);
    EXPECT_EQ(ReasonOf(host, "not a pdf"), "cannotOpen");

    const auto pdf = fixture::TinyPdf({"Offer allopurinol after a first attack."});
    const auto bitmap = host.Render(pdf, 0, 72);
    EXPECT_EQ(bitmap.width, 595);
    EXPECT_EQ(bitmap.height, 842);
    ASSERT_EQ(bitmap.bmp.size(), 54u + 595u * 842u * 4u);
    EXPECT_EQ(bitmap.bmp[0], 'B');
    // White paper, ink somewhere
    EXPECT_EQ(bitmap.bmp[54], 0xFF);
    bool ink = false;
    for (std::size_t i = 54; i + 4 <= bitmap.bmp.size(); i += 4) ink = ink || bitmap.bmp[i] < 0x80;
    EXPECT_TRUE(ink);
    try {
        host.Render(pdf, 1, 72);
        FAIL() << "page 1 does not exist";
    } catch (const HostError& e) {
        EXPECT_EQ(e.Reason(), "badPage");
    }
}

}  // namespace
}  // namespace clinicavt::guidance
