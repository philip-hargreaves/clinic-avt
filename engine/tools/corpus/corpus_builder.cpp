#include "tools/corpus/corpus_builder.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "adapters/guidance/corpus_store.hpp"
#include "adapters/guidance/schema.hpp"
#include "adapters/storage/db.hpp"
#include "adapters/system/sha256.hpp"

namespace clinicavt::guidance {
namespace {

void Require(bool ok, const std::string& why) {
    if (!ok) throw std::invalid_argument("corpus build: " + why);
}

}  // namespace

void BuildCorpus(const std::filesystem::path& dir, const CorpusSpec& spec,
                 const std::vector<Chunk>& chunks, std::span<const float> vectors) {
    Require(!spec.id.empty() && !spec.embedder.id.empty() && spec.embedder.dim > 0,
            "spec incomplete");
    Require(vectors.size() == chunks.size() * static_cast<std::size_t>(spec.embedder.dim),
            "vectors do not match chunks by dim");
    const auto dim = static_cast<std::size_t>(spec.embedder.dim);
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        Require(!chunks[i].text.empty(), "chunk " + chunks[i].id + " has no text");
        double norm = 0;
        for (std::size_t d = 0; d < dim; ++d) {
            norm += static_cast<double>(vectors[i * dim + d]) * vectors[i * dim + d];
        }
        Require(std::fabs(norm - 1.0) <= 1e-3,
                "vector " + std::to_string(i) + " is not unit length");
    }

    std::filesystem::create_directories(dir);
    const auto final_path = dir / kCorpusFile;
    const auto temp_path = dir / (std::string(kCorpusFile) + ".tmp");
    std::filesystem::remove(temp_path);
    const int shard_count = static_cast<int>((chunks.size() + kShardVectors - 1) / kShardVectors);
    {
        store::Db db(temp_path, store::Db::Mode::kBuild);
        db.Exec("PRAGMA page_size=65536");
        db.Exec("PRAGMA journal_mode=DELETE");
        db.Exec("PRAGMA synchronous=OFF");
        db.SetApplicationId(kCorpusApplicationId);
        db.Exec(kCorpusSchemaSql);
        {
            store::Db::Transaction txn(db);
            auto meta = db.Prepare(
                "INSERT INTO corpus_meta(id, corpus_id, name, licence, attribution, source,"
                " embedder_id, embedder_rev, max_tokens, dim, vector_format, normalised,"
                " chunk_count, shard_count, built_at, builder)"
                " VALUES(1, ?, ?, ?, ?, ?, ?, ?, ?, ?, 'f32le', 1, ?, ?, ?, ?)");
            meta.BindText(1, spec.id);
            meta.BindText(2, spec.name);
            meta.BindText(3, spec.licence);
            meta.BindText(4, spec.attribution);
            meta.BindText(5, spec.source);
            meta.BindText(6, spec.embedder.id);
            meta.BindText(7, spec.embedder.rev);
            meta.BindInt64(8, spec.embedder.max_tokens);
            meta.BindInt64(9, spec.embedder.dim);
            meta.BindInt64(10, static_cast<std::int64_t>(chunks.size()));
            meta.BindInt64(11, shard_count);
            meta.BindText(12, spec.built_at);
            meta.BindText(13, spec.builder);
            meta.Step();

            auto insert = db.Prepare(
                "INSERT INTO chunks(ord, chunk_id, code, title, chapter, number, section,"
                " update_tag, last_updated, url, text)"
                " VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
            for (std::size_t i = 0; i < chunks.size(); ++i) {
                const Chunk& c = chunks[i];
                insert.Reset();
                insert.BindInt64(1, static_cast<std::int64_t>(i));
                insert.BindText(2, c.id);
                insert.BindText(3, c.code);
                insert.BindText(4, c.title);
                insert.BindText(5, c.chapter);
                insert.BindText(6, c.number);
                insert.BindText(7, c.section);
                insert.BindText(8, c.update_tag);
                insert.BindText(9, c.last_updated);
                insert.BindText(10, c.url);
                insert.BindText(11, c.text);
                insert.Step();
            }

            auto shard = db.Prepare(
                "INSERT INTO guidance_vectors(shard, first_ord, count, dim, data) VALUES(?, ?, ?, "
                "?, ?)");
            for (int s = 0; s < shard_count; ++s) {
                const std::size_t first = static_cast<std::size_t>(s) * kShardVectors;
                const std::size_t count =
                    std::min<std::size_t>(kShardVectors, chunks.size() - first);
                const auto* bytes =
                    reinterpret_cast<const std::uint8_t*>(vectors.data() + first * dim);
                shard.Reset();
                shard.BindInt64(1, s);
                shard.BindInt64(2, static_cast<std::int64_t>(first));
                shard.BindInt64(3, static_cast<std::int64_t>(count));
                shard.BindInt64(4, spec.embedder.dim);
                shard.BindBlob(5,
                               std::span<const std::uint8_t>(bytes, count * dim * sizeof(float)));
                shard.Step();
            }
            txn.Commit();
        }
        db.SetUserVersion(kCorpusFormat);
        db.Exec("PRAGMA synchronous=FULL");
        db.Exec("VACUUM");
        db.Exec("PRAGMA optimize");
    }
    if (!MoveFileExW(temp_path.c_str(), final_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto error = GetLastError();
        std::filesystem::remove(temp_path);
        throw std::runtime_error("corpus build: rename failed, error " + std::to_string(error));
    }

    nlohmann::json manifest{{"format", kCorpusFormat},
                            {"id", spec.id},
                            {"name", spec.name},
                            {"licence", spec.licence},
                            {"attribution", spec.attribution},
                            {"label", spec.label},
                            {"source", spec.source},
                            {"research", spec.research},
                            {"embedder_id", spec.embedder.id},
                            {"embedder_rev", spec.embedder.rev},
                            {"max_tokens", spec.embedder.max_tokens},
                            {"dim", spec.embedder.dim},
                            {"chunks", chunks.size()},
                            {"shards", shard_count},
                            {"file", kCorpusFile},
                            {"bytes", std::filesystem::file_size(final_path)},
                            {"sha256", system::Sha256File(final_path)},
                            {"built_at", spec.built_at},
                            {"builder", spec.builder}};
    std::ofstream out(dir / kManifestFile, std::ios::binary | std::ios::trunc);
    out << manifest.dump(2) << '\n';
    if (!out) throw std::runtime_error("corpus build: manifest write failed");
}

}  // namespace clinicavt::guidance
