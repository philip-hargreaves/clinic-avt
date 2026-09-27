#pragma once

#include <string>
#include <vector>

#include "ports/diariser.hpp"
#include "ports/transcriber.hpp"

namespace clinicavt::diar {

// Below this lexical margin no one is named. Chosen from the observed
// margin range without a sweep. Revisit first if it abstains too often
inline constexpr double kRoleMinMargin = 0.5;

// Below this the stored print is someone else's and the content decides
// instead. Measured on the 57 with an enrolled print: the enrolled clinician's
// own consultations scored 0.84 and up, every other clinician 0.75 and below
inline constexpr double kAnchorMinSimilarity = 0.80;

struct RoleResult {
    std::vector<std::string> role_of_cluster;  // doctor | patient | speaker N | unknown
    int doctor_cluster = -1;                   // -1: abstained
    int patient_cluster = -1;
    double margin = 0.0;
    bool from_anchor = false;  // the print decided, else the content did or abstained
};

// Cold-start scorer without question features: they invert where the patient
// asks the questions (measured 6/6 -> 0/6)
double LexicalDoctorScore(const std::string& text);

// The two dominant clusters by talk time are the candidates. Anchor rank
// names the doctor, else lexical score with abstention, since a confident
// inversion is the one failure that corrupts the record. texts[i] is the
// text of turns[i], empty when none
RoleResult NameRoles(const std::vector<LabelledSlice>& turns, const std::vector<std::string>& texts,
                     int cluster_count, const std::vector<double>& anchor_similarity = {});

struct NamedTurns {
    RoleResult roles;
    std::vector<asr::Turn> turns;  // the turns with text, each under its cluster's role
};

// Roles named over every turn, then the turns with text attributed. Finalise
// and the capture-phase speculation both read their transcript from here, so
// the prefilled prompt matches the sealed record
NamedTurns NameTurns(const std::vector<LabelledSlice>& turns, const std::vector<std::string>& texts,
                     int cluster_count, const std::vector<double>& anchor_similarity = {});

}  // namespace clinicavt::diar
