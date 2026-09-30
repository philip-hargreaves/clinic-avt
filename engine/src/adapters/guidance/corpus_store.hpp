#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "adapters/guidance/embedder.hpp"
#include "adapters/storage/db.hpp"

namespace clinicavt::guidance {

inline constexpr std::uint32_t kCorpusApplicationId = 0x414D4247;  // "AMBG"
inline constexpr int kCorpusFormat = 1;
inline constexpr int kShardVectors = 256;  // 1 MB a shard at 1024 dimensions
inline constexpr const char* kCorpusFile = "corpus.db";
inline constexpr const char* kManifestFile = "manifest.json";

struct CorpusInfo {
    std::string id;
    std::string name;
    std::string licence;
    std::string attribution;
    std::string label;
    std::string source;
    bool research = false;  // a demo or evaluation corpus, kept out of the package
    std::string embedder_id;
    std::string embedder_rev;
    std::string built_at;
    std::string sha256;  // of corpus.db, from the manifest, verified at open
    int dim = 0;
    std::int64_t chunk_count = 0;
};

// Result fields that need no DB read
struct Cite {
    std::string chunk_id;
    std::string code;
    std::string title;
    std::string number;
    std::string section;
};

struct ChunkText {
    std::string text;
    std::string url;
    std::string last_updated;
    std::string update_tag;
};

// A read-only corpus with its vectors resident as one matrix and text read on demand. Every
// manifest field is checked against the file and the embedder, and a failed check marks the
// corpus unavailable with a reason. Single-threaded
class CorpusStore {
   public:
    static std::unique_ptr<CorpusStore> Open(const std::filesystem::path& dir,
                                             const EmbedderIdentity& embedder, std::string& reason);

    const CorpusInfo& Info() const {
        return info_;
    }
    std::size_t Size() const {
        return cites_.size();
    }
    int Dim() const {
        return info_.dim;
    }
    // Row-major Size() by Dim(), unit vectors
    const float* Matrix() const {
        return matrix_.data();
    }
    const Cite& CiteAt(std::size_t ord) const {
        return cites_[ord];
    }
    ChunkText TextAt(std::size_t ord);

   private:
    CorpusStore(store::Db db, CorpusInfo info);

    store::Db db_;
    CorpusInfo info_;
    std::vector<float> matrix_;
    std::vector<Cite> cites_;
    store::Db::Stmt text_;
};

}  // namespace clinicavt::guidance
