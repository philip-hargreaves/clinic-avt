#include "core/diarisation/turn_decode.hpp"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace clinicavt::diar {
namespace {

const std::vector<float> kAudio(400000, 0.1f);

DecodeClipFn Decoder(std::vector<std::pair<std::uint64_t, std::uint64_t>>* calls = nullptr,
                     std::string reply = "spoken") {
    return [calls, reply](std::span<const float> clip, std::uint64_t first) {
        if (calls != nullptr) calls->push_back({first, clip.size()});
        asr::Turn chunk;
        chunk.first_frame = first;
        chunk.frame_count = clip.size();
        chunk.text = reply + " at " + std::to_string(first);
        return std::vector<asr::Turn>{chunk};
    };
}

TEST(MergeByCluster, ConsecutiveSameClusterSlicesBecomeOneTurn) {
    const std::vector<LabelledSlice> slices{
        {0, 10000, 0}, {12000, 30000, 0}, {31000, 50000, 1}, {52000, 60000, 0}};
    const auto turns = MergeByCluster(slices);
    ASSERT_EQ(turns.size(), 3u);
    EXPECT_EQ(turns[0].first_frame, 0u);
    EXPECT_EQ(turns[0].end_frame, 30000u);
    EXPECT_EQ(turns[1].cluster, 1);
    EXPECT_EQ(turns[2].first_frame, 52000u);
}

using Call = std::pair<std::uint64_t, std::uint64_t>;  // first frame, clip length

TEST(DecodeTurnTexts, EachTurnDecodesOnlyItsOwnUnheardAudio) {
    std::vector<Call> calls;
    const auto texts = DecodeTurnTexts({{0, 30000, 0}, {30000, 60000, 1}}, kAudio, Decoder(&calls));
    ASSERT_EQ(texts.size(), 2u);
    EXPECT_EQ(texts[0], "spoken at 0");
    EXPECT_EQ(texts[1], "spoken at 30000");
    EXPECT_EQ(calls, (std::vector<Call>{{0, 30000}, {30000, 30000}})) << "each its own audio";

    // The second turn starts inside the first. Only its unheard tail decodes
    calls.clear();
    (void)DecodeTurnTexts({{0, 40000, 0}, {30000, 60000, 1}}, kAudio, Decoder(&calls));
    EXPECT_EQ(calls, (std::vector<Call>{{0, 40000}, {40000, 20000}}))
        << "an overlapping head is clamped, not decoded twice";

    // The audio belongs to whoever talked through it. Decoding it would put
    // the louder speaker's words under the quieter speaker's name
    calls.clear();
    const auto nested =
        DecodeTurnTexts({{0, 60000, 0}, {20000, 40000, 1}}, kAudio, Decoder(&calls));
    EXPECT_TRUE(nested[1].empty()) << "a nested overlap turn gets no text";
    EXPECT_EQ(calls.size(), 1u);
}

TEST(DecodeTurnTexts, ClipsAreBoundedToRealAudioAboveTheFloor) {
    std::vector<Call> calls;
    const auto sliver = DecodeTurnTexts({{0, 4000, 0}}, kAudio, Decoder(&calls));  // 0.25 s
    EXPECT_TRUE(sliver[0].empty());
    EXPECT_TRUE(calls.empty()) << "a sub-floor span is skipped, not decoded";

    // A 0.35 s "No." sits between the 0.30 s floor and the old 0.40 s one, where a
    // dropped denial let the note fabricate the opposite answer
    calls.clear();
    const auto no = DecodeTurnTexts({{100000, 105600, 1}}, kAudio, Decoder(&calls, "No."));
    EXPECT_EQ(no[0], "No. at 100000") << "a short answer above the floor must be decoded";
    EXPECT_EQ(calls, (std::vector<Call>{{100000, 5600}}));

    calls.clear();
    (void)DecodeTurnTexts({{390000, 500000, 0}}, kAudio, Decoder(&calls));
    EXPECT_EQ(calls, (std::vector<Call>{{390000, 10000}}))
        << "a turn past the audio end is clamped to the audio that exists";
}

TEST(DecodeTurnTexts, ADegenerateDecodeYieldsEmpty) {
    const std::vector<LabelledSlice> turns{{0, 30000, 0}};
    const auto texts = DecodeTurnTexts(turns, kAudio, [](std::span<const float>, std::uint64_t) {
        asr::Turn chunk;
        chunk.frame_count = 30000;
        chunk.text =
            "the same five words again the same five words again "
            "the same five words again the same five words again";
        return std::vector<asr::Turn>{chunk};
    });
    EXPECT_TRUE(texts[0].empty()) << "a repetition loop has no safe fallback";
}

TEST(DecodeTurnTexts, OnlyAnExactCachedSpanSkipsTheDecode) {
    const std::vector<LabelledSlice> turns{{0, 30000, 0}, {30000, 60000, 1}};
    TurnTexts cache;
    cache[{0, 30000}] = "speculated words";
    // A stale speculation whose boundaries did not survive clustering is never found
    cache[{30000, 59999}] = "stale speculation";
    std::vector<Call> calls;
    const auto texts = DecodeTurnTexts(turns, kAudio, Decoder(&calls), &cache);
    EXPECT_EQ(texts[0], "speculated words");
    EXPECT_EQ(texts[1], "spoken at 30000") << "the stale key misses and the turn decodes fresh";
    EXPECT_EQ(calls, (std::vector<Call>{{30000, 30000}})) << "only the miss decodes";
}

TEST(SpeculatedTurns, TakesTheCachedPrefixSkippingSubFloorSpansAsFinaliseDoes) {
    std::vector<std::string> texts;
    EXPECT_TRUE(SpeculatedTurns({{0, 30000, 0}}, 30000, {}, &texts).empty()) << "nothing known";
    EXPECT_TRUE(texts.empty());

    const std::vector<LabelledSlice> merged{
        {0, 30000, 0}, {30000, 60000, 1}, {60000, 90000, 0}, {90000, 120000, 1}};
    TurnTexts cache;
    cache[{0, 30000}] = "first";
    cache[{30000, 60000}] = "second";
    cache[{90000, 120000}] = "fourth";  // known, but behind an unknown turn
    const auto known = SpeculatedTurns(merged, 120000, cache, &texts);
    ASSERT_EQ(known.size(), 2u) << "stops at the first span the cache does not hold";
    EXPECT_EQ(known[1].cluster, 1);
    EXPECT_EQ(texts, (std::vector<std::string>{"first", "second"}));

    // Finalise never decodes a sub-floor span, so it cannot end the known prefix
    const std::vector<LabelledSlice> sliver{{0, 30000, 0}, {30000, 32000, 1}, {32000, 60000, 0}};
    TurnTexts sliver_cache;
    sliver_cache[{0, 30000}] = "first";
    sliver_cache[{32000, 60000}] = "third";
    texts.clear();
    const auto past = SpeculatedTurns(sliver, 60000, sliver_cache, &texts);
    ASSERT_EQ(past.size(), 2u);
    EXPECT_EQ(past[1].first_frame, 32000u);
    EXPECT_EQ(texts, (std::vector<std::string>{"first", "third"}));
}

TEST(AssembleFromChunks, ACutPieceOnChunkEdgesIsAssembledNotRedecoded) {
    TurnChunks cache;
    asr::Turn c1, c2, c3;
    c1.first_frame = 0;
    c1.frame_count = 40000;
    c1.text = "have you had clots?";
    c2.first_frame = 41000;
    c2.frame_count = 12000;
    c2.text = "No.";
    c3.first_frame = 54000;
    c3.frame_count = 30000;
    c3.text = "Anyone in your family?";
    cache[{0, 84000}] = {c1, c2, c3};
    const auto piece = AssembleFromChunks(cache, 40500, 53500);  // the cut around "No."
    ASSERT_TRUE(piece.has_value());
    EXPECT_EQ(asr::JoinedText(*piece), "No.");
    EXPECT_FALSE(AssembleFromChunks(cache, 20000, 53500).has_value()) << "not on a chunk edge";
}

}  // namespace
}  // namespace clinicavt::diar
