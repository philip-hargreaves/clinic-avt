#include "core/diarisation/tidy_transcript.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace clinicavt::diar {
namespace {

asr::Turn T(std::uint64_t first_s10, std::uint64_t end_s10, const char* who, const char* text) {
    asr::Turn t;
    t.first_frame = first_s10 * 1600;  // tenths of a second
    t.frame_count = (end_s10 - first_s10) * 1600;
    t.speaker = who;
    t.text = text;
    return t;
}

// Short answers like "No." are kept. Only slivers with no content are dropped
TEST(TidyTranscript, DropsContentlessSliversAndPunctuationButKeepsShortAnswers) {
    const auto out = TidyTranscript({T(0, 5, "patient", "um"), T(6, 9, "patient", "uh"),
                                     T(10, 14, "patient", "and"), T(15, 20, "doctor", "No."),
                                     T(21, 25, "patient", "Yes, certainly.")});
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].text, "No.");
    EXPECT_EQ(out[1].text, "Yes, certainly.");

    EXPECT_FALSE(NoContent("um, peanuts"));
    EXPECT_FALSE(NoContent("Okay."));
    EXPECT_FALSE(NoContent("Right."));
    EXPECT_TRUE(NoContent("So I"));
    EXPECT_FALSE(NoContent("so I want to")) << "three function words: a real fragment";

    // A clipped decode can leave a punctuation-only turn, which is dropped and never merged into a
    // neighbour
    const auto clipped = TidyTranscript({T(0, 10, "patient", "I don't know."),
                                         T(11, 12, "patient", "."), T(13, 15, "doctor", "No."),
                                         T(16, 17, "doctor", " ? "), T(18, 20, "patient", "Yes.")});
    ASSERT_EQ(clipped.size(), 3u);
    EXPECT_EQ(clipped[0].text, "I don't know.");
    EXPECT_EQ(clipped[1].text, "No.");
    EXPECT_EQ(clipped[2].text, "Yes.");
}

TEST(TidyTranscript, MergesOneSpeakersNearbyFragmentsIntoSentences) {
    const auto out = TidyTranscript({T(1040, 1046, "doctor", "Right."),
                                     T(1055, 1079, "doctor", "And has this happened before?"),
                                     T(1090, 1100, "patient", "no no i haven't"),
                                     T(1130, 1140, "patient", "not before")});
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].text, "Right. And has this happened before?") << "under a second apart";
    EXPECT_EQ(out[0].first_frame, 1040u * 1600u);
    EXPECT_EQ(out[0].frame_count, (1079u - 1040u) * 1600u);
    EXPECT_EQ(out[1].text, "No no I haven't.") << "3 s gap: not merged";
    EXPECT_EQ(out[2].text, "Not before.");

    // Merging runs before dropping, so a sliver rejoins its own sentence first
    const auto sliver =
        TidyTranscript({T(635, 648, "doctor", "Sure, sure."), T(651, 661, "patient", "So I..."),
                        T(663, 681, "patient", "to know what's going on."),
                        T(685, 711, "doctor", "And so did you say")});
    ASSERT_EQ(sliver.size(), 3u);
    EXPECT_EQ(sliver[1].text, "So I... to know what's going on.");

    const auto finished = TidyTranscript({T(0, 10, "doctor", "Was that a GP or a specialist?"),
                                          T(11, 20, "doctor", "or is it physiotherapist?"),
                                          T(30, 40, "patient", "Do you think"),
                                          T(41, 50, "patient", "it's dangerous?")});
    ASSERT_EQ(finished.size(), 2u);
    EXPECT_EQ(finished[0].text, "Was that a GP or a specialist? Or is it physiotherapist?")
        << "a fragment after a finished sentence is capitalised";
    EXPECT_EQ(finished[1].text, "Do you think it's dangerous?");
}

TEST(TidyTranscript, TurnsStartWithACapitalAndEndWithPunctuation) {
    const auto out = TidyTranscript({T(0, 10, "doctor", "okay sure so you said i think"),
                                     T(20, 30, "patient", "Thank you. OK,"),
                                     T(40, 50, "doctor", "So I..."),  // stranded sliver, dropped
                                     T(60, 70, "patient", "i'm 53")});
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].text, "Okay sure so you said I think.");
    EXPECT_EQ(out[1].text, "Thank you. OK.");
    EXPECT_EQ(out[2].text, "I'm 53.");
}

TEST(TidyTranscript, NoWordMovesBetweenSpeakers) {
    const auto in =
        std::vector<asr::Turn>{T(0, 10, "doctor", "how old are you"), T(11, 14, "patient", "53"),
                               T(15, 30, "doctor", "fantastic and any other illnesses")};
    const auto out = TidyTranscript(in);
    ASSERT_EQ(out.size(), 3u);
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(out[i].speaker, in[i].speaker);
        EXPECT_EQ(out[i].first_frame, in[i].first_frame);
    }
}

}  // namespace
}  // namespace clinicavt::diar
