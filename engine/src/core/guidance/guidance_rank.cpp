#include "core/guidance/guidance_rank.hpp"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/common/strings.hpp"

namespace clinicavt::guidance {

namespace {

std::string Erase(std::string s, std::string_view phrase) {
    for (auto pos = s.find(phrase); pos != std::string::npos; pos = s.find(phrase)) {
        s.erase(pos, phrase.size());
    }
    return s;
}

bool ContainsAny(std::string_view s, std::initializer_list<const char*> needles) {
    for (const char* n : needles) {
        if (strings::Contains(s, n)) return true;
    }
    return false;
}

// Whole-word match, so "female" is not "male" and "woman" is not "man"
bool ContainsWord(std::string_view s, std::string_view word) {
    for (auto pos = s.find(word); pos != std::string_view::npos; pos = s.find(word, pos + 1)) {
        const bool left = pos == 0 || !std::isalnum(static_cast<unsigned char>(s[pos - 1]));
        const auto end = pos + word.size();
        const bool right = end >= s.size() || !std::isalnum(static_cast<unsigned char>(s[end]));
        if (left && right) return true;
    }
    return false;
}

bool ContainsAnyWord(std::string_view s, std::initializer_list<const char*> words) {
    for (const char* w : words) {
        if (ContainsWord(s, w)) return true;
    }
    return false;
}

// The first age the note states ("42-year-old", "65 years old", "aged 72"), or -1
int StatedAge(std::string_view lower) {
    if (const auto aged = lower.find("aged "); aged != std::string_view::npos) {
        int age = 0, digits = 0;
        for (auto k = aged + 5;
             k < lower.size() && std::isdigit(static_cast<unsigned char>(lower[k])) && digits < 3;
             ++k, ++digits) {
            age = age * 10 + (lower[k] - '0');
        }
        if (digits > 0) return age;
    }
    for (std::size_t i = 0; i < lower.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(lower[i]))) continue;
        std::size_t j = i;
        int age = 0;
        while (j < lower.size() && std::isdigit(static_cast<unsigned char>(lower[j])) &&
               j - i < 3) {
            age = age * 10 + (lower[j] - '0');
            ++j;
        }
        const auto rest = lower.substr(j);
        if (rest.starts_with("-year-old") || rest.starts_with(" year old") ||
            rest.starts_with(" years old") || rest.starts_with("-year old") ||
            rest.starts_with("yo ") || rest.starts_with(" yo")) {
            return age;
        }
        i = j;
    }
    return -1;
}

// "children", "under 5s", "young people", "neonatal": text about the young.
// "under 50" is an adult threshold, so only ages up to 18 count
bool AboutChildren(std::string_view lower) {
    if (ContainsAnyWord(lower, {"children", "child", "infant", "infants", "infancy", "neonatal",
                                "neonates", "paediatric", "boy", "boys", "girl", "girls"}) ||
        ContainsAny(lower, {"young people", "adolescent"})) {
        return true;
    }
    for (auto at = lower.find("under "); at != std::string_view::npos;
         at = lower.find("under ", at + 6)) {
        int age = 0;
        std::size_t i = at + 6;
        while (i < lower.size() && std::isdigit(static_cast<unsigned char>(lower[i]))) {
            age = age * 10 + (lower[i] - '0');
            ++i;
        }
        if (i > at + 6 && age <= 18) return true;
    }
    return false;
}

}  // namespace

std::vector<Candidate> RankVote(const std::vector<SubQueryHits>& lists, int limit,
                                double vote_floor) {
    struct Tally {
        double score = 0;
        double cosine = -1;
        std::size_t best_rank = ~std::size_t{0};
        std::string trigger;
    };
    std::map<std::string, Tally> tally;
    for (const auto& list : lists) {
        for (std::size_t rank = 0; rank < list.hits.size(); ++rank) {
            if (!list.whole_note && list.hits[rank].cosine < vote_floor) break;
            auto& t = tally[list.hits[rank].id];
            t.score += 1.0 / (kRrfK + static_cast<double>(rank) + 1.0);
            t.cosine = std::max(t.cosine, list.hits[rank].cosine);
            if (rank < t.best_rank) {
                t.best_rank = rank;
                t.trigger = list.query;
            }
        }
    }
    std::vector<Candidate> out;
    out.reserve(tally.size());
    for (auto& [id, t] : tally) out.push_back({id, t.score, t.cosine, t.trigger});
    std::stable_sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.cosine > b.cosine;
    });
    if (static_cast<int>(out.size()) > limit) out.resize(static_cast<std::size_t>(limit));
    return out;
}

bool NoteClears(const std::vector<SubQueryHits>& lists, double floor) {
    for (const auto& list : lists) {
        if (list.whole_note) return !list.hits.empty() && list.hits.front().cosine >= floor;
    }
    return true;
}

Ordered ApplyFloor(std::vector<Candidate> ranked, double floor) {
    Ordered out;
    out.considered = static_cast<int>(ranked.size());
    for (auto& c : ranked) {
        if (c.cosine >= floor) out.kept.push_back(std::move(c));
    }
    out.abstained = out.kept.empty();
    return out;
}

bool PopulationConflict(std::string_view note, std::string_view recommendation,
                        std::string_view title) {
    const auto n = strings::Lower(note);
    auto r = strings::Lower(recommendation);
    r = Erase(Erase(r, "not pregnant"), "non-pregnant");
    if (ContainsAny(r, {"pregnant", "pregnancy"}) &&
        ContainsAny(n, {"not pregnant", "non-pregnant", "no pregnancy"})) {
        return true;
    }
    const int age = StatedAge(n);
    // "male" and "female" say adult only with no age given and no child in the note
    const bool adult = age >= 18 || ContainsAnyWord(n, {"adult", "adults", "man", "woman"}) ||
                       (age < 0 && !AboutChildren(n) && ContainsAnyWord(n, {"male", "female"}));
    if (adult && (AboutChildren(r) || AboutChildren(strings::Lower(title)))) {
        return true;
    }
    const bool female = ContainsAnyWord(n, {"woman", "women", "female", "she", "her"});
    const bool male = ContainsAnyWord(n, {"man", "men", "male", "he", "his"});
    if (female && !male && ContainsAnyWord(r, {"man", "men", "male", "males"})) return true;
    if (male && !female && ContainsAnyWord(r, {"woman", "women", "female", "females"})) return true;
    return false;
}

bool NearDuplicate(std::string_view a, std::string_view b) {
    const auto words = [](std::string_view s) {
        std::set<std::string> out;
        for (auto& word :
             strings::LowerTokens(s, [](unsigned char c) { return std::isalnum(c) != 0; })) {
            if (word.size() > 3) out.insert(std::move(word));
        }
        return out;
    };
    const auto wa = words(a);
    const auto wb = words(b);
    const auto smaller = std::min(wa.size(), wb.size());
    if (smaller < 5) return false;
    std::size_t shared = 0;
    for (const auto& w : wa) shared += wb.count(w);
    return static_cast<double>(shared) / static_cast<double>(smaller) >= kDuplicateOverlap;
}

std::string Citation(std::string_view code, std::string_view number, std::string_view title) {
    std::string out(code);
    for (auto& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (!number.empty()) out += " " + std::string(number);
    if (!title.empty()) out += ", " + std::string(title);
    return out;
}

}  // namespace clinicavt::guidance
