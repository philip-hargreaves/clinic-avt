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

void SqliteSessionStore::WriteDocument(const SessionId& id, DocumentKind kind,
                                       const Document& document) {
    const ChunkCipher cipher = CipherFor(id);

    Db::Transaction txn(db_);
    std::int64_t revision = FreshSlotSequence();
    bool rewrite = false;
    {
        Db::Stmt previous =
            db_.Prepare("SELECT revision FROM documents WHERE consultation_id = ? AND kind = ?");
        previous.BindText(1, id);
        previous.BindText(2, SpecFor(kind).name);
        if (previous.Step()) {
            revision = previous.ColumnInt64(0) + 1;
            rewrite = true;
        }
    }
    WriteDocumentRow(id, kind, cipher, revision, document);
    txn.Commit();
    if (rewrite) Checkpoint();
}

void SqliteSessionStore::WriteDocumentRow(const SessionId& id, DocumentKind kind,
                                          const ChunkCipher& cipher, std::int64_t revision,
                                          const Document& document) {
    const KindSpec spec = SpecFor(kind);
    const std::vector<std::uint8_t> sealed =
        cipher.Seal(spec.domain, id, static_cast<std::uint64_t>(revision), AsBytes(document.text));
    const bool note = kind == DocumentKind::kNote;
    Db::Stmt upsert = db_.Prepare(
        "INSERT INTO documents(consultation_id, kind, revision, language, encrypted_text, style,"
        " detail, generated_at, edited_at) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)"
        " ON CONFLICT(consultation_id, kind) DO UPDATE SET revision = excluded.revision,"
        " language = excluded.language, encrypted_text = excluded.encrypted_text,"
        " style = excluded.style, detail = excluded.detail,"
        " generated_at = excluded.generated_at, edited_at = excluded.edited_at");
    upsert.BindText(1, id);
    upsert.BindText(2, spec.name);
    upsert.BindInt64(3, revision);
    upsert.BindText(4, document.language);
    upsert.BindBlob(5, sealed);
    upsert.BindTextOrNull(6, note ? document.style : std::string());
    upsert.BindTextOrNull(7, note ? document.detail : std::string());
    upsert.BindTextOrNull(8, document.generated_at);
    upsert.BindTextOrNull(9, document.edited_at);
    upsert.Step();
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
    Db::Stmt remove = db_.Prepare("DELETE FROM documents WHERE consultation_id = ? AND kind = ?");
    remove.BindText(1, id);
    remove.BindText(2, SpecFor(kind).name);
    EraseWhere(remove);
}

Document SqliteSessionStore::ReadDocumentLocked(const SessionId& id, DocumentKind kind) {
    const ChunkCipher cipher = CipherFor(id);
    return ReadDocumentRow(id, kind, cipher).value_or(Document{});
}

std::optional<Document> SqliteSessionStore::ReadDocumentRow(const SessionId& id, DocumentKind kind,
                                                            const ChunkCipher& cipher) {
    const KindSpec spec = SpecFor(kind);
    Db::Stmt select = db_.Prepare(
        "SELECT revision, language, encrypted_text, style, detail, generated_at, edited_at"
        " FROM documents WHERE consultation_id = ? AND kind = ?");
    select.BindText(1, id);
    select.BindText(2, spec.name);
    if (!select.Step()) return std::nullopt;
    Document document;
    document.revision = select.ColumnInt64(0);
    const auto plain = cipher.Open(spec.domain, id, static_cast<std::uint64_t>(document.revision),
                                   select.ColumnBlob(2));
    document.text.assign(plain.begin(), plain.end());
    document.language = select.ColumnText(1);
    document.style = select.ColumnText(3);
    document.detail = select.ColumnText(4);
    document.generated_at = select.ColumnText(5);
    document.edited_at = select.ColumnText(6);
    return document;
}

}  // namespace clinicavt::store
