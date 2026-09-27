#include "adapters/guidance/guidance_record.hpp"

#include <gtest/gtest.h>

namespace clinicavt::guidance {
namespace {

Results Sample() {
    Results out;
    out.considered = 42;
    out.floor = 0.85;
    out.abstained = false;

    Corpus corpus;
    corpus.id = "fixture-nice";
    corpus.name = "Fixture guidance corpus (structured)";
    corpus.licence = "invented";
    corpus.attribution = "none";
    corpus.source = "nice";
    corpus.embedder = "gte-large-int8";
    corpus.sha256 = "e4f1be59be8647759ccd16d916ba9504b464f39a2799cb598ca5f0e4dc779a9f";
    corpus.chunks = 40;
    corpus.built_at = "2026-09-11T00:00:00Z";
    out.searched.push_back(corpus);

    Result full;
    full.corpus = "fixture-nice";
    full.chunk_id = "fx100-1_1_1";
    full.code = "fx100";
    full.number = "1.1.1";
    full.title = "Fictional inflammatory joint disease: assessment and management";
    full.section = "1.1 Referral";
    full.text = "Refer adults with persistent synovitis of undetermined cause to a specialist.";
    full.url = "https://example.test/guidance/fx100/chapter/1-recommendations#fx100-1_1_1";
    full.last_updated = "2020-10-12";
    full.update_tag = "2009, amended 2018";
    full.source = "nice";
    full.citation = "FX100 1.1.1, Fictional inflammatory joint disease: assessment and management";
    full.score = 0.8971234;  // rounded to 3 dp on the wire
    full.trigger = "Examination shows synovitis of several MCP joints.";
    out.shown.push_back(full);

    Result sparse;  // the optional fields empty, to exercise the null round-trip
    sparse.corpus = "fixture-nice";
    sparse.chunk_id = "fx100-1_2_1";
    sparse.code = "fx100";
    sparse.number = "1.2.1";
    sparse.title = "Fictional inflammatory joint disease: assessment and management";
    sparse.section = "1.2 Investigations";
    sparse.text = "Arrange baseline blood tests before the first appointment.";
    sparse.source = "nice";
    sparse.citation =
        "FX100 1.2.1, Fictional inflammatory joint disease: assessment and management";
    sparse.score = 0.861;
    sparse.trigger = "Bloods requested.";
    out.shown.push_back(sparse);

    Result added;  // a passage from an added document, cited by page
    added.corpus = "upload:4870415453308932637";
    added.chunk_id = "upload:4870415453308932637-19";
    added.number = "1.1.3";
    added.title = "NICE gout 2022";
    added.text = "Assess the possibility of septic arthritis.";
    added.source = "upload";
    added.citation = "NICE gout 2022, page 6, 1.1.3 (added 17 Sep 2026)";
    added.score = 0.871;
    added.document = 4870415453308932637;
    added.page = 5;
    added.pages = 36;
    out.shown.push_back(added);
    return out;
}

TEST(GuidanceRecord, ARecordRoundTripsWithEveryFieldIntact) {
    const Record record{Sample(), 7};
    const json wire = ToJson(record);
    EXPECT_EQ(wire["version"], kRecordVersion);
    EXPECT_EQ(wire["noteRevision"], 7);
    EXPECT_EQ(wire["shown"][1]["url"], "") << "empty fields stay empty strings";
    EXPECT_EQ(wire["shown"][1]["updateTag"], "");
    EXPECT_TRUE(wire["searched"][0]["unavailable"].is_null());

    const Record back = RecordFromJson(wire);
    EXPECT_EQ(ToJson(back), wire) << "reading back and writing again changes nothing";
    EXPECT_EQ(back.note_revision, 7);
    const auto& results = back.results;
    EXPECT_EQ(results.floor, 0.85);
    EXPECT_EQ(results.considered, 42);
    ASSERT_EQ(results.searched.size(), 1u);
    EXPECT_EQ(results.searched[0].id, "fixture-nice");
    EXPECT_EQ(results.searched[0].chunks, 40);
    EXPECT_TRUE(results.searched[0].unavailable.empty());
    ASSERT_EQ(results.shown.size(), 3u);
    EXPECT_EQ(results.shown[0].score, 0.897) << "the score is rounded to 3 dp";
    EXPECT_EQ(results.shown[1].url, "");
    EXPECT_EQ(results.shown[2].document, 4870415453308932637) << "every bit of a 63-bit id";
    EXPECT_EQ(results.shown[2].page, 5);
    EXPECT_EQ(results.shown[2].pages, 36);
}

TEST(GuidanceRecord, StoresBytesTheWireWouldAlsoAccept) {
    Record record{Sample(), 1};
    record.results.shown[0].text = "curly quote \x92 from a Windows-1252 file";
    EXPECT_THROW(ToJson(record).dump(), json::type_error);
    const json back = json::parse(Dump(record));
    EXPECT_TRUE(CanRead(back));
    EXPECT_NE(back["shown"][0]["text"].get<std::string>().find("curly quote"), std::string::npos);
}

TEST(GuidanceRecord, RefusesWhatItCannotRead) {
    const json empty{{"shown", json::array()}, {"abstained", false}};
    EXPECT_FALSE(CanRead(json::parse("[]")));
    EXPECT_FALSE(CanRead(json{{"version", kRecordVersion + 1}}));
    EXPECT_FALSE(CanRead(json{{"version", 0}, {"shown", json::array()}, {"abstained", false}}));
    EXPECT_FALSE(CanRead(json{{"version", 1}, {"shown", 5}, {"abstained", false}}));
    EXPECT_FALSE(CanRead(json{{"version", 1}, {"shown", json::array()}, {"abstained", "yes"}}));
    EXPECT_FALSE(CanRead(json{{"version", 1}, {"shown", json::array()}}));
    EXPECT_TRUE(CanRead(empty)) << "a record before version was written";
    EXPECT_EQ(RecordFromJson(empty).note_revision, 0);
}

}  // namespace
}  // namespace clinicavt::guidance
