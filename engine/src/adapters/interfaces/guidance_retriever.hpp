#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace clinicavt::guidance {

// A corpus found by the retriever. Hash, embedder and build date identify the
// exact source text. A corpus failing a load check is listed with the reason and
// not searched
struct Corpus {
    std::string id;
    std::string name;
    std::string licence;
    std::string attribution;
    std::string label;      // the chip's name for the publisher, "NICE", empty when the name serves
    std::string source;     // the importer that built it, "upload" for an added document
    bool research = false;  // a demo or evaluation corpus for dev builds, kept out of the package
    std::string embedder;
    std::string sha256;
    int chunks = 0;
    std::string built_at;     // ISO 8601
    std::string unavailable;  // empty when loaded, otherwise why not
};

// A recommendation for the panel
struct Result {
    std::string corpus;
    std::string chunk_id;  // guideline code and recommendation number, "ng100-1_1_1"
    std::string code;      // the code alone, "ng100"
    std::string number;    // recommendation number, "1.1.1"
    std::string title;
    std::string section;
    std::string text;
    std::string url;
    std::string last_updated;  // ISO 8601, from the guideline
    std::string update_tag;    // the guideline's change marker, "2009, amended 2018"
    std::string source;        // the corpus's source, "upload" for an added document
    std::string citation;      // code, number and title on one line, for the clipboard
    double score = 0;
    std::string trigger;  // the note sentence that found it, empty for the whole note or a query
    std::int64_t document = 0;  // an added document's id, 0 for a corpus
    int page = 0;               // 0-based page of an added PDF, 0 otherwise
    int pages = 0;              // the added PDF's page count, 0 otherwise
};

struct Results {
    std::vector<Result> shown;
    std::vector<Corpus> searched;  // corpora open to the search, for the record
    int considered = 0;            // candidates before the floor and the filters
    double floor = 0;              // cosine floor the candidates were held to
    double upload_floor = 0;       // the same for the added documents
    bool abstained = false;        // nothing to show, by filter or by floor
};

struct Readiness {
    enum class Phase { kLoading, kReady, kUnavailable };
    Phase phase = Phase::kLoading;
    std::string detail;  // the loader's reason when unavailable
};

// kNote splits the note into sentences, drops negated ones and searches each, plus the whole note.
// kQuery searches one query of any length. The floor and population guard apply to both
enum class SearchMode { kNote, kQuery };

// Retrieves guidelines for a note and returns filtered, ranked and thresholded recommendations.
// Retrieved text never goes into a generated document
class IGuidanceRetriever {
   public:
    virtual ~IGuidanceRetriever() = default;

    // Starts slow loading in the background so the first Search is warm. Idempotent
    virtual void Prepare() {}

    virtual Results Search(const std::string& text, int limit, SearchMode mode) = 0;

    virtual std::vector<Corpus> Corpora() = 0;

    virtual Readiness Status() = 0;

    // Dev only: include or exclude corpora marked research, live
    virtual void SetResearch(bool /*include*/) {}
};

}  // namespace clinicavt::guidance
