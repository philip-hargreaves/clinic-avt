#include "adapters/guidance/document_index.hpp"

#include <cstring>
#include <system_error>

#include "adapters/guidance/loader_core.hpp"
#include "adapters/guidance/schema.hpp"
#include "core/common/iso8601.hpp"
#include "core/common/utf8.hpp"
#include "ports/store_error.hpp"

namespace clinicavt::guidance {
namespace {

using store::Db;
using store::StoreCode;
using store::StoreError;

// Opens the file, or deletes what is there and starts again when it is not
// an index of this format
Db OpenIndex(const std::filesystem::path& file) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            Db db(file, Db::Mode::kIndex);
            const auto application_id = db.ApplicationId();
            const auto version = db.UserVersion();
            Guard(application_id == 0 || application_id == kIndexApplicationId, "not an index");
            Guard(version == 0 || version == kIndexFormat, "another format");
            if (version == 0) {
                Guard(db.QueryInt64("SELECT count(*) FROM sqlite_master") == 0, "not an index");
                Db::Transaction txn(db);
                db.Exec(kIndexSchemaSql);
                db.SetApplicationId(kIndexApplicationId);
                db.SetUserVersion(kIndexFormat);
                txn.Commit();
            }
            return db;
        } catch (const std::exception&) {
            if (attempt == 1) throw;
            std::error_code ignored;
            for (const char* suffix : {"", "-wal", "-shm"}) {
                std::filesystem::remove(file.string() + suffix, ignored);
            }
        }
    }
    throw StoreError(StoreCode::kIo, "the index could not be opened");
}

constexpr const char* kSelectRow =
    "SELECT d.id, d.sha256, d.mime, d.state, d.error, d.added_at, d.indexed_at, d.pages,"
    " d.pages_without_text, (SELECT count(*) FROM chunks c WHERE c.document_id = d.id),"
    " (SELECT f.path FROM files f WHERE f.document_id = d.id ORDER BY length(f.path), f.path"
    " LIMIT 1),"
    " (SELECT f.size FROM files f WHERE f.document_id = d.id LIMIT 1) FROM documents d";

DocumentInfo Row(Db::Stmt& select) {
    DocumentInfo info;
    info.id = select.ColumnInt64(0);
    info.sha256 = select.ColumnText(1);
    info.mime = select.ColumnText(2);
    info.state = select.ColumnText(3);
    info.error = select.ColumnText(4);
    info.added_at = select.ColumnText(5);
    info.indexed_at = select.ColumnText(6);
    info.pages = static_cast<int>(select.ColumnInt64(7));
    info.pages_without_text = static_cast<int>(select.ColumnInt64(8));
    info.chunks = select.ColumnInt64(9);
    info.path = select.ColumnText(10);
    info.bytes = select.ColumnInt64(11);
    info.name = utf8::FromPath(utf8::ToPath(info.path).stem());
    return info;
}

constexpr const char* kSelectChunks =
    "SELECT ord, page, number, section, text, vector, boxes FROM chunks WHERE document_id = ?";

IndexChunk ChunkRow(Db::Stmt& select, std::size_t dim) {
    IndexChunk chunk;
    chunk.ord = select.ColumnInt64(0);
    chunk.page = static_cast<int>(select.ColumnInt64(1));
    chunk.number = select.ColumnText(2);
    chunk.section = select.ColumnText(3);
    chunk.text = select.ColumnText(4);
    const auto vec = select.ColumnBlobView(5);
    Guard(vec.size() == dim * sizeof(float), "a stored vector has the wrong length");
    chunk.vector.resize(dim);
    std::memcpy(chunk.vector.data(), vec.data(), vec.size());
    chunk.boxes = select.ColumnText(6);
    return chunk;
}

}  // namespace

std::int64_t DocumentIndex::IdOf(const std::string& sha256) {
    const auto id = static_cast<std::int64_t>(std::stoull(sha256.substr(0, 16), nullptr, 16) &
                                              0x7FFFFFFFFFFFFFFFull);
    return id == 0 ? 1 : id;
}

DocumentIndex::DocumentIndex(const std::filesystem::path& file) : db_(OpenIndex(file)) {
    auto meta = db_.Prepare(
        "SELECT embedder_id, embedder_rev, dim, max_tokens FROM index_meta WHERE id = 1");
    if (meta.Step()) {
        embedder_ = {meta.ColumnText(0), meta.ColumnText(1), static_cast<int>(meta.ColumnInt64(2)),
                     static_cast<int>(meta.ColumnInt64(3))};
    }
}

void DocumentIndex::Adopt(const EmbedderIdentity& embedder) {
    if (embedder_ != embedder) {
        Clear();
        Db::Transaction txn(db_);
        db_.Exec("DELETE FROM index_meta");
        auto insert = db_.Prepare(
            "INSERT INTO index_meta(id, embedder_id, embedder_rev, dim, max_tokens, created_at)"
            " VALUES(1, ?, ?, ?, ?, ?)");
        insert.BindText(1, embedder.id);
        insert.BindText(2, embedder.rev);
        insert.BindInt64(3, embedder.dim);
        insert.BindInt64(4, embedder.max_tokens);
        insert.BindText(5, Iso8601Now());
        insert.Step();
        txn.Commit();
        embedder_ = embedder;
    }
    adopted_ = true;
}

std::vector<DocumentInfo> DocumentIndex::List() {
    std::vector<DocumentInfo> out;
    auto select = db_.Prepare((std::string(kSelectRow) + " ORDER BY d.added_at, d.id").c_str());
    while (select.Step()) out.push_back(Row(select));
    return out;
}

DocumentInfo DocumentIndex::Get(std::int64_t id) {
    auto select = db_.Prepare((std::string(kSelectRow) + " WHERE d.id = ?").c_str());
    select.BindInt64(1, id);
    if (!select.Step()) throw StoreError(StoreCode::kNotFound, "no document " + std::to_string(id));
    return Row(select);
}

std::vector<IndexedFile> DocumentIndex::Files() {
    std::vector<IndexedFile> out;
    auto select = db_.Prepare("SELECT path, document_id, size, modified FROM files");
    while (select.Step()) {
        out.push_back({select.ColumnText(0), select.ColumnInt64(1), select.ColumnInt64(2),
                       select.ColumnInt64(3)});
    }
    return out;
}

std::vector<std::string> DocumentIndex::PathsOf(std::int64_t id) {
    std::vector<std::string> out;
    auto select = db_.Prepare("SELECT path FROM files WHERE document_id = ? ORDER BY path");
    select.BindInt64(1, id);
    while (select.Step()) out.push_back(select.ColumnText(0));
    return out;
}

DocumentIndex::Held DocumentIndex::Hold(const IndexedFile& file, const std::string& sha256,
                                        const std::string& mime) {
    Held held;
    held.document = IdOf(sha256);
    Db::Transaction txn(db_);
    auto before = db_.Prepare("SELECT document_id FROM files WHERE path = ?");
    before.BindText(1, file.path);
    const std::int64_t previous = before.Step() ? before.ColumnInt64(0) : 0;
    before.Reset();

    auto exists = db_.Prepare("SELECT 1 FROM documents WHERE id = ?");
    exists.BindInt64(1, held.document);
    if (!exists.Step()) {
        auto insert = db_.Prepare(
            "INSERT INTO documents(id, sha256, mime, state, added_at)"
            " VALUES(?, ?, ?, 'indexing', ?)");
        insert.BindInt64(1, held.document);
        insert.BindText(2, sha256);
        insert.BindText(3, mime);
        insert.BindText(4, Iso8601Now());
        insert.Step();
        held.added = true;
    }
    exists.Reset();

    auto upsert = db_.Prepare(
        "INSERT INTO files(path, document_id, size, modified) VALUES(?, ?, ?, ?)"
        " ON CONFLICT(path) DO UPDATE SET document_id = excluded.document_id,"
        " size = excluded.size, modified = excluded.modified");
    upsert.BindText(1, file.path);
    upsert.BindInt64(2, held.document);
    upsert.BindInt64(3, file.size);
    upsert.BindInt64(4, file.modified);
    upsert.Step();

    if (previous != 0 && previous != held.document) held.released = DropUnheld(previous);
    txn.Commit();
    if (held.released != 0) db_.CheckpointTruncate();
    return held;
}

std::int64_t DocumentIndex::Release(const std::string& path) {
    Db::Transaction txn(db_);
    auto held = db_.Prepare("SELECT document_id FROM files WHERE path = ?");
    held.BindText(1, path);
    if (!held.Step()) return 0;
    const auto document = held.ColumnInt64(0);
    held.Reset();
    auto erase_file = db_.Prepare("DELETE FROM files WHERE path = ?");
    erase_file.BindText(1, path);
    erase_file.Step();
    const auto released = DropUnheld(document);
    txn.Commit();
    if (released != 0) db_.CheckpointTruncate();
    return released;
}

void DocumentIndex::Finish(std::int64_t id, const std::vector<IndexChunk>& chunks, int pages,
                           int pages_without_text) {
    const auto dim = static_cast<std::size_t>(embedder_.dim);
    Db::Transaction txn(db_);
    ClearChunks(id);
    auto insert = db_.Prepare(
        "INSERT INTO chunks(document_id, ord, page, number, section, text, vector, boxes)"
        " VALUES(?, ?, ?, ?, ?, ?, ?, ?)");
    for (std::size_t ord = 0; ord < chunks.size(); ++ord) {
        const auto& chunk = chunks[ord];
        if (chunk.vector.size() != dim)
            throw std::invalid_argument("vector has the wrong dimension");
        insert.Reset();
        insert.BindInt64(1, id);
        insert.BindInt64(2, static_cast<std::int64_t>(ord));
        insert.BindInt64(3, chunk.page);
        insert.BindText(4, chunk.number);
        insert.BindText(5, chunk.section);
        insert.BindText(6, chunk.text);
        insert.BindBlob(
            7, {reinterpret_cast<const std::uint8_t*>(chunk.vector.data()), dim * sizeof(float)});
        insert.BindText(8, chunk.boxes);
        insert.Step();
    }
    auto ready = db_.Prepare(
        "UPDATE documents SET state = 'ready', error = NULL, indexed_at = ?, pages = ?,"
        " pages_without_text = ? WHERE id = ?");
    ready.BindText(1, Iso8601Now());
    ready.BindInt64(2, pages);
    ready.BindInt64(3, pages_without_text);
    ready.BindInt64(4, id);
    ready.Step();
    if (db_.QueryInt64("SELECT changes()") == 0) {
        throw StoreError(StoreCode::kNotFound, "no document " + std::to_string(id));
    }
    txn.Commit();
}

void DocumentIndex::Fail(std::int64_t id, const std::string& error, int pages,
                         int pages_without_text) {
    Db::Transaction txn(db_);
    ClearChunks(id);
    auto failed = db_.Prepare(
        "UPDATE documents SET state = 'failed', error = ?, pages = ?, pages_without_text = ?"
        " WHERE id = ?");
    failed.BindText(1, error);
    failed.BindInt64(2, pages);
    failed.BindInt64(3, pages_without_text);
    failed.BindInt64(4, id);
    failed.Step();
    if (db_.QueryInt64("SELECT changes()") == 0) {
        throw StoreError(StoreCode::kNotFound, "no document " + std::to_string(id));
    }
    txn.Commit();
}

std::vector<IndexChunk> DocumentIndex::ReadChunks(std::int64_t id) {
    std::vector<IndexChunk> out;
    auto select = db_.Prepare((std::string(kSelectChunks) + " ORDER BY ord").c_str());
    select.BindInt64(1, id);
    while (select.Step()) out.push_back(ChunkRow(select, static_cast<std::size_t>(embedder_.dim)));
    return out;
}

IndexChunk DocumentIndex::ReadChunk(std::int64_t id, std::int64_t ord) {
    auto select = db_.Prepare((std::string(kSelectChunks) + " AND ord = ?").c_str());
    select.BindInt64(1, id);
    select.BindInt64(2, ord);
    if (!select.Step()) {
        throw StoreError(StoreCode::kNotFound, "no chunk " + std::to_string(ord));
    }
    return ChunkRow(select, static_cast<std::size_t>(embedder_.dim));
}

void DocumentIndex::Clear() {
    db_.Exec("DELETE FROM documents");
    db_.CheckpointTruncate();
}

void DocumentIndex::ClearChunks(std::int64_t id) {
    auto clear = db_.Prepare("DELETE FROM chunks WHERE document_id = ?");
    clear.BindInt64(1, id);
    clear.Step();
}

std::int64_t DocumentIndex::DropUnheld(std::int64_t document) {
    auto still = db_.Prepare("SELECT 1 FROM files WHERE document_id = ?");
    still.BindInt64(1, document);
    if (still.Step()) return 0;
    auto erase = db_.Prepare("DELETE FROM documents WHERE id = ?");
    erase.BindInt64(1, document);
    erase.Step();
    return document;
}

}  // namespace clinicavt::guidance
