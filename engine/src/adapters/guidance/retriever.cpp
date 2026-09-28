#include "adapters/guidance/retriever.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>

#include "core/common/strings.hpp"
#include "core/guidance/guidance_query.hpp"
#include "core/guidance/guidance_scan.hpp"

namespace clinicavt::guidance {
namespace {

bool IsResearch(const std::filesystem::path& manifest) {
    try {
        std::ifstream in(manifest);
        return nlohmann::json::parse(in).value("research", false);
    } catch (const std::exception&) {
        return false;
    }
}

Corpus Describe(const CorpusInfo& info) {
    Corpus c;
    c.id = info.id;
    c.name = info.name;
    c.licence = info.licence;
    c.attribution = info.attribution;
    c.label = info.label;
    c.source = info.source;
    c.research = info.research;
    c.embedder = info.embedder_id;
    c.sha256 = info.sha256;
    c.chunks = static_cast<int>(info.chunk_count);
    c.built_at = info.built_at;
    return c;
}

// Hit position across corpora, used as the vote key
struct Located {
    std::size_t corpus = 0;
    std::size_t ord = 0;
    float cosine = 0;
};

std::string Key(const Located& l) {
    return std::to_string(l.corpus) + ":" + std::to_string(l.ord);
}

// "15 Sep 2026" from "2026-09-15T09:12:44Z", empty from anything shorter
std::string ShortDate(const std::string& iso) {
    static const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (iso.size() < 10) return "";
    const int month = std::atoi(iso.substr(5, 2).c_str());
    const int day = std::atoi(iso.substr(8, 2).c_str());
    if (month < 1 || month > 12 || day < 1) return "";
    return std::to_string(day) + " " + kMonths[month - 1] + " " + iso.substr(0, 4);
}

// "BSR PMR guidelines 2009, page 3, 1.2 (added 15 Sep 2026)"
std::string UploadCitation(const UploadSnapshot::Row& row) {
    std::string out = row.name;
    if (row.pages > 0) out += ", page " + std::to_string(row.page + 1);
    if (!row.number.empty()) out += ", " + row.number;
    const auto added = ShortDate(row.added_at);
    if (!added.empty()) out += " (added " + added + ")";
    return out;
}

}  // namespace

Retriever::Retriever(EmbedderLoader load_embedder, std::filesystem::path corpora_root,
                     RetrieverOptions options)
    : load_embedder_(std::move(load_embedder)),
      corpora_root_(std::move(corpora_root)),
      options_(options) {}

void Retriever::Prepare() {
    std::lock_guard<std::mutex> lock(search_mutex_);
    Load();
}

void Retriever::Load() {
    if (embedder_) return;
    if (!load_error_.empty()) throw std::runtime_error(load_error_);
    try {
        auto embedder = load_embedder_();
        if (!embedder) throw std::runtime_error("no guidance embedder");
        embedder_ = std::move(embedder);
        LoadCorpora();
    } catch (const std::exception& e) {
        load_error_ = e.what();
        std::lock_guard<std::mutex> lock(corpora_mutex_);
        readiness_ = {Readiness::Phase::kUnavailable, load_error_};
        throw;
    }
}

// Rescans the corpora folder with the research setting and swaps in the set.
// Keeps the embedder
void Retriever::LoadCorpora() {
    std::vector<std::filesystem::path> dirs;
    if (std::filesystem::is_directory(corpora_root_)) {
        for (const auto& entry : std::filesystem::directory_iterator(corpora_root_)) {
            const auto manifest = entry.path() / kManifestFile;
            if (!entry.is_directory() || !std::filesystem::exists(manifest)) continue;
            // Research corpora are demos, skipped unless a dev build asks
            if (!options_.include_research && IsResearch(manifest)) continue;
            dirs.push_back(entry.path());
        }
    }
    std::sort(dirs.begin(), dirs.end());
    std::vector<Loaded> loaded;
    std::vector<Corpus> corpora;
    for (const auto& dir : dirs) {
        Loaded item;
        std::string reason;
        item.store = CorpusStore::Open(dir, embedder_->Identity(), reason);
        if (item.store) {
            item.corpus = Describe(item.store->Info());
        } else {
            item.corpus.id = dir.filename().string();
            item.corpus.unavailable = reason;
        }
        corpora.push_back(item.corpus);
        loaded.push_back(std::move(item));
    }
    loaded_ = std::move(loaded);
    std::lock_guard<std::mutex> lock(corpora_mutex_);
    corpora_ = std::move(corpora);
    readiness_ = {Readiness::Phase::kReady, ""};
}

void Retriever::SetResearch(bool include) {
    std::lock_guard<std::mutex> lock(search_mutex_);
    if (options_.include_research == include) return;
    options_.include_research = include;
    if (embedder_) LoadCorpora();
}

Embedding Retriever::Embed(const std::string& text) {
    std::lock_guard<std::mutex> lock(search_mutex_);
    Load();
    return embedder_->Embed(text);
}

EmbedderIdentity Retriever::Identity() {
    std::lock_guard<std::mutex> lock(search_mutex_);
    Load();
    return embedder_->Identity();
}

void Retriever::PublishUploads(std::shared_ptr<const UploadSnapshot> uploads) {
    std::lock_guard<std::mutex> lock(search_mutex_);
    uploads_ = std::move(uploads);
}

namespace {

// Near-duplicate of a shown card, so not worth a slot
bool Restates(const std::vector<Result>& shown, const std::string& text) {
    for (const auto& result : shown) {
        if (NearDuplicate(result.text, text)) return true;
    }
    return false;
}

}  // namespace

Results Retriever::Search(const std::string& text, int limit, SearchMode mode) {
    std::lock_guard<std::mutex> lock(search_mutex_);
    Load();
    Results out;
    out.floor = options_.floor;
    out.upload_floor = options_.upload_floor;
    const auto uploads = uploads_;
    std::vector<CorpusStore*> stores;
    if (uploads) out.searched = uploads->documents;
    for (auto& item : loaded_) {
        if (item.store) {
            stores.push_back(item.store.get());
            out.searched.push_back(item.corpus);
        }
    }
    const std::string whole(strings::Trim(text));
    std::vector<std::string> queries;
    if (mode == SearchMode::kQuery) {
        if (!whole.empty()) queries.push_back(whole);
    } else {
        queries = SubQueries(text);
    }
    if (queries.empty()) {
        out.abstained = true;
        return out;
    }
    const bool have_uploads = uploads && !uploads->rows.empty();
    if (stores.empty() && !have_uploads) return out;

    // Each sub-query is embedded once and scanned against both groups
    std::vector<std::pair<std::string, Embedding>> embedded;
    embedded.reserve(queries.size());
    for (const auto& query : queries) embedded.emplace_back(query, embedder_->Embed(query));

    struct Source {
        const float* matrix;
        std::size_t size;
        int dim;
    };
    const int k = kUnionSize;
    // Rank each group per sub-query before the vote. A narrow group returns
    // nothing unless the whole note clears its floor, and a sentence votes only
    // at or above that floor
    const auto vote = [&](const std::vector<Source>& sources, double floor,
                          std::map<std::string, Located>& where, bool strict) {
        std::vector<SubQueryHits> lists;
        for (const auto& [query, embedding] : embedded) {
            std::vector<Located> located;
            for (std::size_t c = 0; c < sources.size(); ++c) {
                const auto hits = Scan(sources[c].matrix, sources[c].size, sources[c].dim,
                                       embedding.vector.data(), k);
                for (const auto& hit : hits) located.push_back({c, hit.ord, hit.cosine});
            }
            std::stable_sort(
                located.begin(), located.end(),
                [](const Located& a, const Located& b) { return a.cosine > b.cosine; });
            if (located.size() > static_cast<std::size_t>(k))
                located.resize(static_cast<std::size_t>(k));
            SubQueryHits list{query, query == whole, {}};
            for (const auto& l : located) {
                auto key = Key(l);
                list.hits.push_back({key, l.cosine});
                where.emplace(std::move(key), l);
            }
            lists.push_back(std::move(list));
        }
        if (strict && !NoteClears(lists, options_.upload_note_floor)) {
            Ordered silent;
            silent.abstained = true;
            return silent;
        }
        return ApplyFloor(RankVote(lists, k, strict ? floor : -1.0), floor);
    };

    // Hits failing the population guard, or restating a shown card, take no slot
    const auto suppressed = [&](const std::string& body, const std::string& title) {
        if (PopulationConflict(text, body, title)) {
            std::fprintf(stderr, "clinicavt-engine: guard suppressed a hit in %s\n", title.c_str());
            return true;
        }
        return Restates(out.shown, body);
    };

    // Added documents lead, as their own group with their own floor
    if (have_uploads) {
        std::map<std::string, Located> where;
        const auto ordered = vote({{uploads->matrix.data(), uploads->rows.size(), uploads->dim}},
                                  options_.upload_floor, where, true);
        out.considered += ordered.considered;
        int shown = 0;
        for (const auto& candidate : ordered.kept) {
            if (shown >= limit) break;
            const auto& at = where.at(candidate.id);
            const auto& row = uploads->rows[at.ord];
            if (suppressed(row.text, row.name)) continue;
            Result result;
            result.corpus = "upload:" + std::to_string(row.document);
            result.chunk_id = result.corpus + "-" + std::to_string(row.ord);
            result.number = row.number;
            result.title = row.name;
            result.section = row.section;
            result.citation = UploadCitation(row);
            result.text = row.text;
            result.last_updated = row.added_at;
            result.source = "upload";
            result.score = candidate.cosine;
            result.trigger = candidate.trigger == whole ? "" : candidate.trigger;
            result.document = row.document;
            result.page = row.page;
            result.pages = row.pages;
            out.shown.push_back(std::move(result));
            ++shown;
        }
    }

    if (!stores.empty()) {
        std::vector<Source> sources;
        sources.reserve(stores.size());
        for (auto* store : stores)
            sources.push_back({store->Matrix(), store->Size(), store->Dim()});
        std::map<std::string, Located> where;
        const auto ordered = vote(sources, options_.floor, where, false);
        out.considered += ordered.considered;
        int shown = 0;
        for (const auto& candidate : ordered.kept) {
            if (shown >= limit) break;
            const auto& at = where.at(candidate.id);
            auto* store = stores[at.corpus];
            auto chunk = store->TextAt(at.ord);
            const auto& cite = store->CiteAt(at.ord);
            if (suppressed(chunk.text, cite.title)) continue;
            Result result;
            result.corpus = store->Info().id;
            result.chunk_id = cite.chunk_id;
            result.code = cite.code;
            result.number = cite.number;
            result.title = cite.title;
            result.section = cite.section;
            result.citation = Citation(cite.code, cite.number, cite.title);
            result.text = std::move(chunk.text);
            result.url = std::move(chunk.url);
            result.last_updated = std::move(chunk.last_updated);
            result.update_tag = std::move(chunk.update_tag);
            result.source = store->Info().source;
            result.score = candidate.cosine;
            result.trigger = candidate.trigger == whole ? "" : candidate.trigger;
            out.shown.push_back(std::move(result));
            ++shown;
        }
    }
    out.abstained = out.shown.empty();
    return out;
}

std::vector<Corpus> Retriever::Corpora() {
    std::lock_guard<std::mutex> lock(corpora_mutex_);
    return corpora_;
}

Readiness Retriever::Status() {
    std::lock_guard<std::mutex> lock(corpora_mutex_);
    return readiness_;
}

}  // namespace clinicavt::guidance
