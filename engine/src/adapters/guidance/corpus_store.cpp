#include "adapters/guidance/corpus_store.hpp"

#include <cmath>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "adapters/guidance/loader_core.hpp"
#include "adapters/system/sha256.hpp"

namespace clinicavt::guidance {
namespace {

std::uint64_t AvailablePhysicalMemory() {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof status;
    return GlobalMemoryStatusEx(&status) ? status.ullAvailPhys : 0;
}

// Size limit for the matrix and one citation per row, checked before allocating
void GuardMemory(std::size_t rows, std::size_t dim, const std::string& what) {
    const auto needed = static_cast<std::uint64_t>(rows) * dim * sizeof(float) + rows * 256;
    Guard(AvailablePhysicalMemory() > needed, what + " too large for the available memory");
}

void GuardUnitVectors(const float* matrix, std::size_t rows, std::size_t dim) {
    for (std::size_t r = 0; r < rows; ++r) {
        double norm = 0;
        const float* v = matrix + r * dim;
        for (std::size_t d = 0; d < dim; ++d) norm += static_cast<double>(v[d]) * v[d];
        Guard(std::fabs(norm - 1.0) <= 1e-3, "a vector is not unit length");
    }
}

void GuardFormat(std::int64_t format, const std::string& what) {
    Guard(format >= kCorpusFormat,
          what + " format " + std::to_string(format) + " is older than this build reads");
    Guard(format <= kCorpusFormat,
          what + " format " + std::to_string(format) + " is newer than this build reads");
}

std::string Str(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string())
        throw Refused(std::string("manifest: ") + key + " missing");
    return it->get<std::string>();
}

std::int64_t Int(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) {
        throw Refused(std::string("manifest: ") + key + " missing");
    }
    return it->get<std::int64_t>();
}

std::string Differs(const char* field) {
    return std::string(field) + " differs between file and manifest";
}

}  // namespace

CorpusStore::CorpusStore(store::Db db, CorpusInfo info)
    : db_(std::move(db)),
      info_(std::move(info)),
      text_(db_.Prepare(
          "SELECT text, url, last_updated, update_tag FROM passages WHERE position = ?")) {}

std::unique_ptr<CorpusStore> CorpusStore::Open(const std::filesystem::path& dir,
                                               const EmbedderIdentity& embedder,
                                               std::string& reason) {
    try {
        std::ifstream in(dir / kManifestFile);
        Guard(in.is_open(), "no manifest.json");
        nlohmann::json manifest;
        try {
            manifest = nlohmann::json::parse(in);
        } catch (const std::exception&) {
            throw Refused("manifest.json does not parse");
        }
        GuardFormat(Int(manifest, "format"), "manifest");
        CorpusInfo info;
        info.id = Str(manifest, "id");
        info.name = Str(manifest, "name");
        info.licence = Str(manifest, "licence");
        info.attribution = Str(manifest, "attribution");
        info.label = manifest.value("label", std::string());
        info.source = Str(manifest, "source");
        info.research = manifest.value("research", false);
        info.embedder_id = Str(manifest, "embedder_id");
        info.embedder_rev = Str(manifest, "embedder_rev");
        info.built_at = Str(manifest, "built_at");
        info.sha256 = Str(manifest, "sha256");
        info.dim = static_cast<int>(Int(manifest, "dim"));
        const auto max_tokens = Int(manifest, "max_tokens");
        const auto builder = Str(manifest, "builder");
        const auto bytes = Int(manifest, "bytes");

        const auto path = dir / manifest.value("file", kCorpusFile);
        Guard(std::filesystem::exists(path), "corpus.db missing");
        Guard(static_cast<std::int64_t>(std::filesystem::file_size(path)) == bytes,
              "corpus.db size differs from the manifest");
        Guard(system::Sha256File(path) == info.sha256, "corpus.db hash differs from the manifest");

        store::Db db(path, store::Db::Mode::kImmutableReadOnly);
        Guard(db.ApplicationId() == static_cast<std::int64_t>(kCorpusApplicationId),
              "not a corpus file");
        GuardFormat(db.UserVersion(), "corpus");
        {
            // Header bytes 18-19 are the format versions (1 rollback journal, 2 WAL). An
            // immutable connection does not report the journal mode, so read the bytes
            std::ifstream header(path, std::ios::binary);
            char format[20] = {};
            header.read(format, sizeof format);
            Guard(header.gcount() == sizeof format && format[18] == 1 && format[19] == 1,
                  "corpus file was left in WAL mode");
        }

        auto meta = db.Prepare(
            "SELECT corpus_id, name, licence, attribution, source, embedder_id, embedder_rev,"
            " max_tokens, dim, built_at, builder FROM corpus_metadata WHERE id = 1");
        Guard(meta.Step(), "corpus_metadata has no row");
        Guard(meta.ColumnText(0) == info.id, Differs("corpus id"));
        Guard(meta.ColumnText(1) == info.name, Differs("name"));
        Guard(meta.ColumnText(2) == info.licence, Differs("licence"));
        Guard(meta.ColumnText(3) == info.attribution, Differs("attribution"));
        Guard(meta.ColumnText(4) == info.source, Differs("source"));
        Guard(meta.ColumnText(5) == info.embedder_id && meta.ColumnText(6) == info.embedder_rev,
              Differs("embedder"));
        Guard(meta.ColumnInt64(7) == max_tokens, Differs("max tokens"));
        Guard(meta.ColumnInt64(8) == info.dim, Differs("dimension"));
        Guard(meta.ColumnText(9) == info.built_at, Differs("build time"));
        Guard(meta.ColumnText(10) == builder, Differs("builder"));
        Guard(!meta.Step(), "corpus_metadata has more than one row");
        Guard(info.embedder_id == embedder.id && info.embedder_rev == embedder.rev,
              "built with " + info.embedder_id + " " + info.embedder_rev.substr(0, 12) +
                  ", the staged embedder is " + embedder.id + " " + embedder.rev.substr(0, 12));
        Guard(info.dim == embedder.dim, "dimension differs from the staged embedder");
        Guard(max_tokens == embedder.max_tokens, "max tokens differs from the pipeline's");

        info.passage_count = db.QueryInt64("SELECT count(*) FROM passages");
        if (info.passage_count > 0) {
            Guard(db.QueryInt64("SELECT min(position) FROM passages") == 0 &&
                      db.QueryInt64("SELECT max(position) FROM passages") == info.passage_count - 1,
                  "passage positions have gaps");
        }

        const auto dim = static_cast<std::size_t>(info.dim);
        const auto rows = static_cast<std::size_t>(info.passage_count);
        GuardMemory(rows, dim, "corpus");

        auto store = std::unique_ptr<CorpusStore>(new CorpusStore(std::move(db), info));
        store->matrix_.resize(rows * dim);
        store->cites_.reserve(rows);

        auto shards = store->db_.Prepare(
            "SELECT shard, first_position, passage_count, vectors FROM vector_shards"
            " ORDER BY shard");
        std::int64_t expected_shard = 0, next_position = 0;
        while (shards.Step()) {
            Guard(shards.ColumnInt64(0) == expected_shard, "shards are not numbered 0..n-1");
            const auto first = shards.ColumnInt64(1);
            const auto count = shards.ColumnInt64(2);
            Guard(first == next_position, "shards are not contiguous");
            const auto data = shards.ColumnBlobView(3);
            Guard(data.size() == static_cast<std::size_t>(count) * dim * sizeof(float),
                  "a shard has the wrong length");
            Guard(first + count <= info.passage_count,
                  "shards cover more rows than there are passages");
            std::memcpy(store->matrix_.data() + static_cast<std::size_t>(first) * dim, data.data(),
                        data.size());
            next_position = first + count;
            ++expected_shard;
        }
        Guard(next_position == info.passage_count, "shards do not cover every passage");

        GuardUnitVectors(store->matrix_.data(), rows, dim);

        auto cites = store->db_.Prepare(
            "SELECT passage_id, code, title, number, section FROM passages ORDER BY position");
        while (cites.Step()) {
            store->cites_.push_back({cites.ColumnText(0), cites.ColumnText(1), cites.ColumnText(2),
                                     cites.ColumnText(3), cites.ColumnText(4)});
        }
        Guard(store->cites_.size() == rows, "passage rows changed under the reader");
        reason.clear();
        return store;
    } catch (const Refused& e) {
        reason = e.what();
    } catch (const std::exception& e) {
        reason = std::string("corpus unreadable: ") + e.what();
    }
    return nullptr;
}

ChunkText CorpusStore::TextAt(std::size_t ord) {
    text_.Reset();
    text_.BindInt64(1, static_cast<std::int64_t>(ord));
    if (!text_.Step()) throw std::out_of_range("no passage at position " + std::to_string(ord));
    ChunkText out{text_.ColumnText(0), text_.ColumnText(1), text_.ColumnText(2),
                  text_.ColumnText(3)};
    text_.Reset();
    return out;
}

}  // namespace clinicavt::guidance
