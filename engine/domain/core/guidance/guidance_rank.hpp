#pragma once

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace clinicavt::guidance {

// Ordering and abstention over first-stage candidates. Each sub-query's
// ranked hits vote by rank (reciprocal rank fusion), a floor on the best cosine
// refuses out-of-scope input, and a population guard drops recommendations the
// note rules out
inline constexpr double kRrfK = 60.0;

inline constexpr double kDefaultFloor = 0.85;

inline constexpr double kNoteFloor = 0.84;  // the whole note against a narrow group

inline constexpr int kUnionSize = 50;

struct Hit {
    std::string id;
    double cosine = 0;
};

// One sub-query's hits in rank order
struct SubQueryHits {
    std::string query;
    bool whole_note = false;
    std::vector<Hit> hits;
};

struct Candidate {
    std::string id;
    double score = 0;     // fused vote
    double cosine = 0;    // best cosine over the sub-queries
    std::string trigger;  // the sub-query that ranked it highest
};

struct Ordered {
    std::vector<Candidate> kept;
    int considered = 0;
    bool abstained = false;
};

// Sentence hits under vote_floor don't vote. Whole-note hits always do
std::vector<Candidate> RankVote(const std::vector<SubQueryHits>& lists, int limit = kUnionSize,
                                double vote_floor = -1.0);

// True when the whole-note top hit reaches floor, or when there is no whole-note list. Sentences
// are ignored because one can match a passage in any document
bool NoteClears(const std::vector<SubQueryHits>& lists, double floor);

// Drops candidates whose best cosine is under floor. abstained is set when none remain
Ordered ApplyFloor(std::vector<Candidate> ranked, double floor = kDefaultFloor);

// True when the recommendation targets a population the note explicitly excludes, such as pregnancy
// against "not pregnant", children against a stated adult, or the other sex. Checks the title too,
// since a recommendation rarely restates e.g. under-5s. Only explicit statements count
bool PopulationConflict(std::string_view note, std::string_view recommendation,
                        std::string_view title = "");

// True when half the shorter passage's content words appear in the other, as when a quality
// standard restates its guideline
inline constexpr double kDuplicateOverlap = 0.5;

bool NearDuplicate(std::string_view a, std::string_view b);

// "NG100 1.1.1, Rheumatoid arthritis in adults: management"
std::string Citation(std::string_view code, std::string_view number, std::string_view title);

}  // namespace clinicavt::guidance
