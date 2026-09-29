#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "adapters/guidance/corpus_store.hpp"
#include "adapters/guidance/embedder.hpp"
#include "adapters/interfaces/guidance_retriever.hpp"
#include "core/guidance/guidance_rank.hpp"

namespace clinicavt::guidance {

struct RetrieverOptions {
    double floor = kDefaultFloor;
    double upload_floor = kDefaultFloor;
    double upload_note_floor = kNoteFloor;
    bool include_research = false;  // dev only
};

// Every ready added document as one matrix plus rows. Ingest builds it and swaps it in whole, so a
// search never reads the store
struct UploadSnapshot {
    struct Row {
        std::int64_t document = 0;
        std::int64_t ord = 0;  // the chunk's ordinal within its document
        int page = 0;
        int pages = 0;  // the document's page count, 0 for text
        std::string name;
        std::string number;
        std::string section;
        std::string text;
        std::string added_at;
    };
    int dim = 0;
    std::vector<float> matrix;  // rows.size() by dim
    std::vector<Row> rows;
    std::vector<Corpus> documents;  // one per document, for the record's searched list
};

using EmbedderLoader = std::function<std::unique_ptr<IEmbedder>()>;

// Uses one embedder and every corpus under corpora_root that passes load checks. Searches by exact
// scan with a rank vote across sub-queries, a cosine floor and a population guard. Score is the
// best cosine and order follows the vote. Prepare and Search are serialised. Corpora and Status are
// thread-safe. A load failure is cached and rethrown, since the model store does not change at
// runtime
class Retriever : public IGuidanceRetriever {
   public:
    Retriever(EmbedderLoader load_embedder, std::filesystem::path corpora_root,
              RetrieverOptions options = {});

    void Prepare() override;
    Results Search(const std::string& text, int limit, SearchMode mode) override;
    std::vector<Corpus> Corpora() override;
    Readiness Status() override;

    // Used by ingest. Serialised with searches
    Embedding Embed(const std::string& text);
    EmbedderIdentity Identity();
    void PublishUploads(std::shared_ptr<const UploadSnapshot> uploads);

    void SetResearch(bool include) override;

   private:
    struct Loaded {
        Corpus corpus;
        std::unique_ptr<CorpusStore> store;  // null when unavailable
    };
    void Load();
    void LoadCorpora();

    EmbedderLoader load_embedder_;
    std::filesystem::path corpora_root_;
    RetrieverOptions options_;

    std::mutex search_mutex_;
    std::unique_ptr<IEmbedder> embedder_;
    std::vector<Loaded> loaded_;
    std::shared_ptr<const UploadSnapshot> uploads_;
    std::string load_error_;

    std::mutex corpora_mutex_;
    std::vector<Corpus> corpora_;
    Readiness readiness_;
};

}  // namespace clinicavt::guidance
