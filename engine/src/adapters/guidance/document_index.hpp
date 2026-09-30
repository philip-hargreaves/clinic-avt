#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "adapters/guidance/embedder.hpp"
#include "adapters/interfaces/document_ingest.hpp"
#include "adapters/storage/db.hpp"

namespace clinicavt::guidance {

inline constexpr std::uint32_t kIndexApplicationId = 0x414D4249;  // "AMBI"
inline constexpr int kIndexFormat = 4;
inline constexpr const char* kIndexFile = "index.db";

struct IndexedFile {
    std::string path;  // relative to the folder, UTF-8
    std::int64_t document = 0;
    std::int64_t size = 0;
    std::int64_t modified = 0;  // last write time in file clock ticks
};

struct IndexChunk {
    std::int64_t ord = 0;
    int page = 0;
    std::string number;
    std::string section;
    std::string text;
    std::vector<float> vector;  // unit length, the index's dimension
    std::string boxes;          // line boxes as page fractions, JSON
};

// Cache of the guidelines folder. It holds documents by content, their paths and passage vectors.
// Deleted and rebuilt on a format or embedder mismatch. Single-threaded
class DocumentIndex {
   public:
    explicit DocumentIndex(const std::filesystem::path& file);

    // Adopting a different embedder empties the index
    void Adopt(const EmbedderIdentity& embedder);
    bool Adopted() const {
        return adopted_;
    }
    const EmbedderIdentity& Embedder() const {
        return embedder_;
    }

    std::vector<DocumentInfo> List();
    DocumentInfo Get(std::int64_t id);
    std::vector<IndexedFile> Files();
    std::vector<std::string> PathsOf(std::int64_t id);

    // Maps the path to this content. added is set for a new document. released is the id of the
    // path's previous document if no path holds it any more
    struct Held {
        std::int64_t document = 0;
        bool added = false;
        std::int64_t released = 0;
    };
    Held Hold(const IndexedFile& file, const std::string& sha256, const std::string& mime);
    // Unmaps the path. Returns the id of a document left with no path, else 0
    std::int64_t Release(const std::string& path);

    // Writes passages in one transaction and marks the document ready
    void Finish(std::int64_t id, const std::vector<IndexChunk>& chunks, int pages,
                int pages_without_text);
    void Fail(std::int64_t id, const std::string& error, int pages = 0, int pages_without_text = 0);
    std::vector<IndexChunk> ReadChunks(std::int64_t id);
    IndexChunk ReadChunk(std::int64_t id, std::int64_t ord);

    // The first 63 bits of the sha256, with 0 mapped to 1, so the same bytes at any
    // path are one document
    static std::int64_t IdOf(const std::string& sha256);

   private:
    void Clear();
    void ClearChunks(std::int64_t id);
    // Returns the dropped id, else 0
    std::int64_t DropUnheld(std::int64_t document);

    store::Db db_;
    EmbedderIdentity embedder_;
    bool adopted_ = false;
};

}  // namespace clinicavt::guidance
