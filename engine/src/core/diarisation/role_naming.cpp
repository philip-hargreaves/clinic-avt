#include "core/diarisation/role_naming.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include "core/common/strings.hpp"

namespace clinicavt::diar {

namespace {

// Lowercase tokens, apostrophes kept, so "i'm" and "we'll" compare exactly
std::vector<std::string> WordsOf(const std::string& text) {
    return strings::LowerTokens(text, [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '\'';
    });
}

bool In(const std::vector<std::string>& set, const std::string& word) {
    return std::find(set.begin(), set.end(), word) != set.end();
}

// Consecutive whole tokens only: "we can" must not fire
// inside "we cancelled"
bool HasPhrase(const std::vector<std::string>& words, const std::vector<std::string>& phrase) {
    if (phrase.empty() || words.size() < phrase.size()) return false;
    for (std::size_t i = 0; i + phrase.size() <= words.size(); ++i) {
        std::size_t j = 0;
        while (j < phrase.size() && words[i + j] == phrase[j]) ++j;
        if (j == phrase.size()) return true;
    }
    return false;
}

// The clinician naming themselves, as "dr" or "doctor" since ASR output varies
const std::vector<std::vector<std::string>> kIdentPhrases = {
    {"i'm", "dr"},
    {"i'm", "doctor"},
    {"i", "am", "dr"},
    {"i", "am", "doctor"},
    {"this", "is", "dr"},
    {"this", "is", "doctor"},
    {"my", "name", "is", "dr"},
    {"my", "name", "is", "doctor"},
    {"from", "gp", "at", "hand"},
    {"calling", "from"},
};

// Plan speech is all first-person. Unnamed, the penalty would score the
// doctor's most characteristic speech as the patient's
const std::vector<std::vector<std::string>> kPlanPhrases = {
    {"i'll"},
    {"i", "will"},
    {"i'd", "like"},
    {"i", "would"},
    {"i'm", "going", "to"},
    {"i", "want", "you", "to"},
    {"we'll"},
    {"we", "will"},
    {"we're", "going", "to"},
    {"we", "can"},
    {"let's"},
};

}  // namespace

double LexicalDoctorScore(const std::string& text) {
    const auto words = WordsOf(text);
    if (words.empty()) return 0.0;
    const double n = static_cast<double>(words.size());

    static const std::vector<std::string> kFirst = {"i", "my", "me", "i'm", "i've"};
    static const std::vector<std::string> kSecond = {"you", "your", "you're"};
    double first = 0.0, second = 0.0;
    for (const auto& word : words) {
        if (In(kFirst, word)) ++first;
        if (In(kSecond, word)) ++second;
    }

    double bonus = 0.0;
    for (const auto& phrase : kIdentPhrases) {
        if (HasPhrase(words, phrase)) {
            bonus += 8.0;
            first = 0.0;
            break;
        }
    }
    int plan = 0;
    for (const auto& phrase : kPlanPhrases) {
        if (HasPhrase(words, phrase)) ++plan;
    }
    if (plan > 0) {
        first = std::max(0.0, first - plan);
        bonus += 2.5 * std::min(plan, 3);  // capped: one plan-heavy turn cannot dominate
    }
    return bonus + 7.0 * (second / n) - 11.0 * (first / n);
}

RoleResult NameRoles(const std::vector<LabelledSlice>& turns, const std::vector<std::string>& texts,
                     int cluster_count, const std::vector<double>& anchor_similarity) {
    RoleResult result;
    if (cluster_count < 1) return result;
    result.role_of_cluster.assign(static_cast<std::size_t>(cluster_count), "unknown");

    std::vector<double> talk(static_cast<std::size_t>(cluster_count), 0.0);
    for (const auto& turn : turns) {
        if (turn.cluster >= 0 && turn.cluster < cluster_count) {
            talk[static_cast<std::size_t>(turn.cluster)] +=
                static_cast<double>(turn.end_frame - turn.first_frame);
        }
    }
    std::vector<int> order(static_cast<std::size_t>(cluster_count));
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return talk[static_cast<std::size_t>(a)] > talk[static_cast<std::size_t>(b)];
    });
    const int top1 = order[0];
    const int top2 = cluster_count > 1 ? order[1] : order[0];

    int doctor;
    const bool anchored =
        anchor_similarity.size() == static_cast<std::size_t>(cluster_count) && top1 != top2 &&
        std::max(anchor_similarity[static_cast<std::size_t>(top1)],
                 anchor_similarity[static_cast<std::size_t>(top2)]) >= kAnchorMinSimilarity;
    if (anchored) {
        const double sim1 = anchor_similarity[static_cast<std::size_t>(top1)];
        const double sim2 = anchor_similarity[static_cast<std::size_t>(top2)];
        result.margin = std::abs(sim1 - sim2);
        result.from_anchor = true;
        doctor = sim1 >= sim2 ? top1 : top2;  // relative match once someone resembles the print
    } else {
        double sum1 = 0.0, sum2 = 0.0;
        int n1 = 0, n2 = 0;
        for (std::size_t i = 0; i < turns.size(); ++i) {
            if (texts[i].empty()) continue;  // no lexical evidence, would dilute the mean
            const double score = LexicalDoctorScore(texts[i]);
            if (turns[i].cluster == top1) {
                sum1 += score;
                ++n1;
            }
            if (turns[i].cluster == top2) {
                sum2 += score;
                ++n2;
            }
        }
        // Means rather than sums, since turn counts are near 50/50 and carry no signal
        const double mean1 = n1 > 0 ? sum1 / n1 : 0.0;
        const double mean2 = n2 > 0 ? sum2 / n2 : 0.0;
        result.margin = std::abs(mean1 - mean2);
        if (result.margin < kRoleMinMargin || top1 == top2) {
            // Numbered rather than unknown: the separation is still certain,
            // and only the roles are undecided
            result.role_of_cluster[static_cast<std::size_t>(top1)] = "speaker 1";
            if (top2 != top1) result.role_of_cluster[static_cast<std::size_t>(top2)] = "speaker 2";
            return result;
        }
        doctor = mean1 >= mean2 ? top1 : top2;
    }

    const int patient = doctor == top1 ? top2 : top1;
    result.doctor_cluster = doctor;
    result.patient_cluster = patient;
    result.role_of_cluster[static_cast<std::size_t>(doctor)] = "doctor";
    if (patient != doctor) result.role_of_cluster[static_cast<std::size_t>(patient)] = "patient";
    return result;
}

NamedTurns NameTurns(const std::vector<LabelledSlice>& turns, const std::vector<std::string>& texts,
                     int cluster_count, const std::vector<double>& anchor_similarity) {
    NamedTurns named;
    named.roles = NameRoles(turns, texts, cluster_count, anchor_similarity);
    const auto& roles = named.roles.role_of_cluster;
    for (std::size_t i = 0; i < turns.size(); ++i) {
        if (texts[i].empty()) continue;
        const auto cluster = static_cast<std::size_t>(turns[i].cluster);
        asr::Turn turn;
        turn.first_frame = turns[i].first_frame;
        turn.frame_count = turns[i].end_frame - turns[i].first_frame;
        turn.speaker = cluster < roles.size() ? roles[cluster] : "unknown";
        turn.text = texts[i];
        named.turns.push_back(std::move(turn));
    }
    return named;
}

}  // namespace clinicavt::diar
