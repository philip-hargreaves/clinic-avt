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

// A sentence's hit under vote_floor casts no vote: a sentence speaks only for passages it
// resembles. The whole note always votes, since it carries the topic
std::vector<Candidate> RankVote(const std::vector<SubQueryHits>& lists, int limit = kUnionSize,
                                double vote_floor = -1.0);

// Whether the note as a whole resembles anything searched. One sentence can
// resemble a passage of any document, so it cannot say the documents cover the note
bool NoteClears(const std::vector<SubQueryHits>& lists, double floor);

// Candidates whose best cosine is under the floor are dropped. When none
// remains the search abstains and the panel shows nothing
Ordered ApplyFloor(std::vector<Candidate> ranked, double floor = kDefaultFloor);

// A recommendation addressed to a population the note explicitly rules out:
// pregnancy against "not pregnant", children against a stated adult, one sex
// against the other. The guideline title counts as well as the text, since a
// recommendation for under-5s rarely says so itself. Only explicit statements
// count, silence never does
bool PopulationConflict(std::string_view note, std::string_view recommendation,
                        std::string_view title = "");

// Two passages saying the same thing, as a quality standard restates its
// guideline: half the shorter one's content words appear in the other
inline constexpr double kDuplicateOverlap = 0.5;

bool NearDuplicate(std::string_view a, std::string_view b);

// "NG100 1.1.1, Rheumatoid arthritis in adults: management"
std::string Citation(std::string_view code, std::string_view number, std::string_view title);

}  // namespace clinicavt::guidance
