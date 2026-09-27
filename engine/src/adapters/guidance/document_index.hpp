#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "adapters/guidance/embedder.hpp"
#include "adapters/storage/db.hpp"
#include "ports/document_ingest.hpp"

namespace clinicavt::guidance {

inline constexpr std::uint32_t kIndexApplicationId = 0x414D4249;  // "AMBI"
inline constexpr int kIndexFormat = 3;
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

// What the app derived from the files in the guidelines folder: documents by
// content, the paths holding each, and their passages with vectors. A cache:
// a file of another format or embedder is deleted and made again. One thread
// at a time
class DocumentIndex {
   public:
    explicit DocumentIndex(const std::filesystem::path& file);

    // The embedder the passages come from. Another one empties the index
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

    // The path now holds the content. A new document starts indexing and
    // `added` says so. The document the path held before, if nothing holds it
    // now, goes and its id comes back in `released`
    struct Held {
        std::int64_t document = 0;
        bool added = false;
        std::int64_t released = 0;
    };
    Held Hold(const IndexedFile& file, const std::string& sha256, const std::string& mime);
    // The path holds nothing. A document left without a file goes, its id
    // returned, else 0
    std::int64_t Release(const std::string& path);

    // The passages land in one transaction and the document turns ready
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
    // Drops the document when no file holds it, returning its id, else 0
    std::int64_t DropUnheld(std::int64_t document);

    store::Db db_;
    EmbedderIdentity embedder_;
    bool adopted_ = false;
};

}  // namespace clinicavt::guidance
