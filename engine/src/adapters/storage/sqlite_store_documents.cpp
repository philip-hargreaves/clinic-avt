#include <optional>
#include <string>

#include "adapters/storage/sqlite_session_store.hpp"
#include "adapters/storage/sqlite_store_rows.hpp"
#include "core/common/iso8601.hpp"

namespace clinicavt::store {

using rows::AsBytes;
using rows::FreshSlotSequence;
using rows::KindSpec;
using rows::SpecFor;

// Text is encrypted, metadata is not. The options row cascades from the note
// row, so a kind is written all or nothing
void SqliteSessionStore::WriteDocument(const SessionId& id, DocumentKind kind,
                                       const Document& document) {
    const ChunkCipher cipher = CipherFor(id);

    Db::Transaction txn(db_);
    // Slot's next sequence so a rewrite never reuses an IV
    std::int64_t seq = FreshSlotSequence();
    {
        Db::Stmt previous =
            db_.Prepare("SELECT seq FROM documents WHERE session_id = ? AND kind = ?");
        previous.BindText(1, id);
        previous.BindText(2, SpecFor(kind).name);
        if (previous.Step()) seq = previous.ColumnInt64(0) + 1;
    }
    WriteDocumentRow(id, kind, cipher, seq, document);
    txn.Commit();
}

void SqliteSessionStore::WriteDocumentRow(const SessionId& id, DocumentKind kind,
                                          const ChunkCipher& cipher, std::int64_t seq,
                                          const Document& document) {
    const KindSpec spec = SpecFor(kind);
    const std::vector<std::uint8_t> sealed =
        cipher.Seal(spec.domain, id, static_cast<std::uint64_t>(seq), AsBytes(document.text));
    Db::Stmt replace = db_.Prepare(
        "INSERT OR REPLACE INTO documents"
        "(session_id, kind, seq, language, payload, generated_at, edited_at)"
        " VALUES(?, ?, ?, ?, ?, ?, ?)");
    replace.BindText(1, id);
    replace.BindText(2, spec.name);
    replace.BindInt64(3, seq);
    replace.BindText(4, document.language);
    replace.BindBlob(5, sealed);
    replace.BindTextOrNull(6, document.generated_at);
    replace.BindTextOrNull(7, document.edited_at);
    replace.Step();
    if (kind == DocumentKind::kNote) {
        Db::Stmt options = db_.Prepare(
            "INSERT OR REPLACE INTO note_options(session_id, style, detail) VALUES(?, ?, ?)");
        options.BindText(1, id);
        options.BindText(2, document.style);
        options.BindText(3, document.detail);
        options.Step();
    }
}

void SqliteSessionStore::SaveDocument(const SessionId& id, DocumentKind kind,
                                      const Document& document) {
    std::lock_guard<std::mutex> lock(mutex_);
    Document generated = document;
    generated.generated_at = Iso8601Now();
    generated.edited_at.clear();
    WriteDocument(id, kind, generated);
}

void SqliteSessionStore::EditDocument(const SessionId& id, DocumentKind kind,
                                      const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    Document edited = ReadDocumentLocked(id, kind);
    edited.text = text;
    edited.edited_at = Iso8601Now();
    WriteDocument(id, kind, edited);
}

Document SqliteSessionStore::ReadDocument(const SessionId& id, DocumentKind kind) {
    std::lock_guard<std::mutex> lock(mutex_);
    return ReadDocumentLocked(id, kind);
}

void SqliteSessionStore::DeleteDocument(const SessionId& id, DocumentKind kind) {
    std::lock_guard<std::mutex> lock(mutex_);
    RequireStored(id);
    Db::Stmt remove = db_.Prepare("DELETE FROM documents WHERE session_id = ? AND kind = ?");
    remove.BindText(1, id);
    remove.BindText(2, SpecFor(kind).name);
    remove.Step();
}

Document SqliteSessionStore::ReadDocumentLocked(const SessionId& id, DocumentKind kind) {
    const ChunkCipher cipher = CipherFor(id);
    return ReadDocumentRow(id, kind, cipher).value_or(Document{});
}

std::optional<Document> SqliteSessionStore::ReadDocumentRow(const SessionId& id, DocumentKind kind,
                                                            const ChunkCipher& cipher) {
    const KindSpec spec = SpecFor(kind);
    Db::Stmt select = db_.Prepare(
        "SELECT d.language, d.payload, d.generated_at, d.edited_at, o.style, o.detail, d.seq"
        " FROM documents d LEFT JOIN note_options o ON o.session_id = d.session_id"
        " WHERE d.session_id = ? AND d.kind = ?");
    select.BindText(1, id);
    select.BindText(2, spec.name);
    if (!select.Step()) return std::nullopt;
    Document document;
    const auto plain = cipher.Open(
        spec.domain, id, static_cast<std::uint64_t>(select.ColumnInt64(6)), select.ColumnBlob(1));
    document.text.assign(plain.begin(), plain.end());
    document.language = select.ColumnText(0);
    document.generated_at = select.ColumnText(2);
    document.edited_at = select.ColumnText(3);
    document.revision = select.ColumnInt64(6);
    if (kind == DocumentKind::kNote) {
        document.style = select.ColumnText(4);
        document.detail = select.ColumnText(5);
    }
    return document;
}

}  // namespace clinicavt::store
