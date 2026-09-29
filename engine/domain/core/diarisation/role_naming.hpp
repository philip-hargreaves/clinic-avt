#pragma once

#include <string>
#include <vector>

#include "ports/diariser.hpp"
#include "ports/transcriber.hpp"

namespace clinicavt::diar {

// Below this lexical margin no one is named. Chosen from the observed
// margin range without a sweep. Revisit first if it abstains too often
inline constexpr double kRoleMinMargin = 0.5;

// Below this the print is treated as another clinician's and lexical scoring is used. On 57
// enrolled consultations: own clinician >= 0.84, others <= 0.75
inline constexpr double kAnchorMinSimilarity = 0.80;

struct RoleResult {
    std::vector<std::string> role_of_cluster;  // doctor | patient | speaker N | unknown
    int doctor_cluster = -1;                   // -1: abstained
    int patient_cluster = -1;
    double margin = 0.0;
    bool from_anchor = false;  // named from the voice print rather than lexical score
};

// Cold-start scorer without question features: they invert where the patient
// asks the questions (measured 6/6 -> 0/6)
double LexicalDoctorScore(const std::string& text);

// Picks doctor/patient from the two clusters with most talk time: by voice print if it
// matches, else lexical score. Abstains below kRoleMinMargin since swapped roles corrupt the
// record. texts[i] is turns[i]'s text, may be empty
RoleResult NameRoles(const std::vector<LabelledSlice>& turns, const std::vector<std::string>& texts,
                     int cluster_count, const std::vector<double>& anchor_similarity = {});

struct NamedTurns {
    RoleResult roles;
    std::vector<asr::Turn> turns;  // turns with text, speaker set to the cluster's role
};

// Names roles, then labels the turns with text. Shared by finalise and capture-phase
// speculation so the prefilled prompt matches the final record
NamedTurns NameTurns(const std::vector<LabelledSlice>& turns, const std::vector<std::string>& texts,
                     int cluster_count, const std::vector<double>& anchor_similarity = {});

}  // namespace clinicavt::diar
