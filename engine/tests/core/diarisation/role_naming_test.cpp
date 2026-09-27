#include "core/diarisation/role_naming.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace clinicavt::diar {
namespace {

struct RoleTurn {
    int cluster = 0;
    std::uint64_t frames = 0;
    std::string text;
};

RoleTurn Turn(int cluster, std::string text, std::uint64_t frames = 32000) {
    return {cluster, frames, std::move(text)};
}

// The turns laid end to end, as the diariser hands them over
std::vector<LabelledSlice> Slices(const std::vector<RoleTurn>& turns) {
    std::vector<LabelledSlice> slices;
    std::uint64_t at = 0;
    for (const auto& turn : turns) {
        slices.push_back({at, at + turn.frames, turn.cluster});
        at += turn.frames;
    }
    return slices;
}

std::vector<std::string> Texts(const std::vector<RoleTurn>& turns) {
    std::vector<std::string> texts;
    for (const auto& turn : turns) texts.push_back(turn.text);
    return texts;
}

RoleResult NameRoles(const std::vector<RoleTurn>& turns, int cluster_count,
                     const std::vector<double>& anchor_similarity = {}) {
    return diar::NameRoles(Slices(turns), Texts(turns), cluster_count, anchor_similarity);
}

TEST(LexicalDoctorScore, SelfIdentificationScoresAsTheClinician) {
    // The named failure the suppression exists for: the old rule scored
    // this utterance -0.31, below the patient's greeting
    const double doctor =
        LexicalDoctorScore("I'm Doctor Deen Mirza from GP at Hand. Nice to see you.");
    const double patient = LexicalDoctorScore("Nice to see you.");
    EXPECT_GT(doctor, patient);
    EXPECT_GT(doctor, 0.0);
}

TEST(LexicalDoctorScore, PlanSpeechScoresClinicianOnWholeTokensOnly) {
    EXPECT_GT(LexicalDoctorScore("I'll get the form sent out"), 0.0);
    EXPECT_GT(LexicalDoctorScore("we're going to arrange a follow-up for you"), 0.0);
    EXPECT_LT(LexicalDoctorScore("I feel dizzy and my chest hurts"), 0.0);
    // "we can" must not fire inside "we cancelled"
    EXPECT_LT(LexicalDoctorScore("we cancelled because i was unwell"),
              LexicalDoctorScore("we can start the treatment"));
}

std::vector<RoleTurn> Consultation() {
    return {
        Turn(0, "I'm Doctor Mirza, how are you feeling today"),
        Turn(0, "have you noticed any swelling in your knee"),
        Turn(0, "I'll send you the referral form, you should hear next week"),
        Turn(1, "I've had this pain in my knee for about three weeks"),
        Turn(1, "my leg aches when I climb the stairs"),
        Turn(1, "I think I hurt it when I was running"),
    };
}

TEST(NameRoles, TheColdStartNamesDoctorAndPatient) {
    const auto result = NameRoles(Consultation(), 2);
    ASSERT_EQ(result.doctor_cluster, 0);
    EXPECT_EQ(result.patient_cluster, 1);
    EXPECT_EQ(result.role_of_cluster[0], "doctor");
    EXPECT_EQ(result.role_of_cluster[1], "patient");
    EXPECT_GE(result.margin, kRoleMinMargin);
}

TEST(NameRoles, AQuestionAskingPatientCannotInvertTheDecision) {
    auto turns = Consultation();
    for (auto& turn : turns) {  // move every question mark to the patient
        if (turn.cluster == 1) turn.text += "?";
    }
    const auto inverted = NameRoles(turns, 2);
    const auto normal = NameRoles(Consultation(), 2);
    EXPECT_EQ(inverted.doctor_cluster, normal.doctor_cluster);
    EXPECT_EQ(inverted.margin, normal.margin) << "invariant by construction, not empirically";
}

TEST(NameRoles, AbstainsToNumberedSpeakersOnAThinMarginOrOneCluster) {
    const std::vector<RoleTurn> greetings{Turn(0, "good morning"), Turn(1, "good morning")};
    const auto thin = NameRoles(greetings, 2);
    EXPECT_EQ(thin.doctor_cluster, -1) << "a thin margin";
    EXPECT_EQ(thin.role_of_cluster[0], "speaker 1");
    EXPECT_EQ(thin.role_of_cluster[1], "speaker 2");

    const auto single = NameRoles({Turn(0, "hello there, how can I help")}, 1);
    EXPECT_EQ(single.doctor_cluster, -1) << "a single cluster";
    EXPECT_EQ(single.role_of_cluster[0], "speaker 1");
}

TEST(NameRoles, ClusterMeansDecideAndEmptyTurnsDoNotDilute) {
    // Two strong clinician turns against six mild patient turns. A sum
    // would let the count vote, the mean must not
    std::vector<RoleTurn> turns{
        Turn(0, "I'll arrange the scan and I want you to rest your knee"),
        Turn(0, "you should take the tablets with your evening meal"),
    };
    for (int i = 0; i < 6; ++i) turns.push_back(Turn(1, "I see okay"));
    EXPECT_EQ(NameRoles(turns, 2).doctor_cluster, 0) << "means, not turn counts";

    auto padded = Consultation();
    for (int i = 0; i < 20; ++i) padded.push_back(Turn(0, ""));
    EXPECT_EQ(NameRoles(padded, 2).doctor_cluster, 0)
        << "empty turns must not dilute the cluster mean";
}

TEST(NameRoles, AThirdClusterIsNeverACandidate) {
    auto turns = Consultation();
    turns.push_back(Turn(2, "is it serious doctor", 8000));  // brief companion
    const auto result = NameRoles(turns, 3);
    EXPECT_EQ(result.role_of_cluster[2], "unknown");
    EXPECT_EQ(result.role_of_cluster[0], "doctor");
}

TEST(NameRoles, AResemblingPrintDecidesAndAForeignPrintYieldsToContent) {
    // Someone resembles the print: it names the lexically patient-looking
    // cluster as the doctor, content-blind, rank not margin
    const auto resembling = NameRoles(Consultation(), 2, std::vector<double>{0.41, 0.84});
    EXPECT_TRUE(resembling.from_anchor);
    EXPECT_EQ(resembling.doctor_cluster, 1);
    EXPECT_EQ(resembling.role_of_cluster[1], "doctor");
    EXPECT_EQ(resembling.role_of_cluster[0], "patient");

    // Nobody in the room resembles the print (another clinician's): the
    // content decides as if there were no print, so a stale print cannot
    // invert the record
    const auto foreign = NameRoles(Consultation(), 2, std::vector<double>{0.41, 0.44});
    EXPECT_FALSE(foreign.from_anchor);
    EXPECT_EQ(foreign.doctor_cluster, NameRoles(Consultation(), 2).doctor_cluster);
    EXPECT_EQ(foreign.role_of_cluster[0], "doctor");
}

// Finalise and speculation read the same transcript: a turn without text
// still counts as talk time but is not attributed
TEST(NameTurns, AttributesTheTurnsWithTextUnderTheirRoles) {
    auto turns = Consultation();
    turns.insert(turns.begin() + 1, Turn(1, "", 64000));
    const auto named = NameTurns(Slices(turns), Texts(turns), 2);
    EXPECT_EQ(named.roles.doctor_cluster, NameRoles(turns, 2).doctor_cluster);
    ASSERT_EQ(named.turns.size(), Consultation().size()) << "the empty turn is dropped";
    EXPECT_EQ(named.turns[0].speaker, "doctor");
    EXPECT_EQ(named.turns[0].text, Consultation()[0].text);
    EXPECT_EQ(named.turns[1].first_frame, 32000u + 64000u) << "frames kept from the slice";
    EXPECT_EQ(named.turns[1].frame_count, 32000u);
    EXPECT_EQ(named.turns.back().speaker, "patient");
}

}  // namespace
}  // namespace clinicavt::diar
