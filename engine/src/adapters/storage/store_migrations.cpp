#include "adapters/storage/store_migrations.hpp"

#include <map>
#include <string>
#include <vector>

#include "adapters/storage/chunk_cipher.hpp"
#include "adapters/storage/schema.hpp"
#include "adapters/storage/sqlite_store_rows.hpp"
#include "core/common/log.hpp"

namespace clinicavt::store {
namespace {

std::filesystem::path DatabasePath(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    return root / "clinicavt.db";
}

Domain DomainOf(const std::string& kind) {
    for (const DocumentKind k : kDocumentKinds) {
        if (kind == rows::SpecFor(k).name) return rows::SpecFor(k).domain;
    }
    throw StoreError(StoreCode::kSchema, "unknown document kind " + kind);
}

// Version 7 needs a revision above 0. Documents carried through the old 5 to 6 upgrade were
// sealed at 0, so each is resealed at a fresh sequence under its own key. The key and every
// other payload are unchanged. A document that does not open at 0 was already unreadable and
// is deleted
void ResealAtSequenceZero(Db& db) {
    struct Row {
        std::string session;
        std::string kind;
        std::vector<std::uint8_t> payload;
        std::vector<std::uint8_t> wrapped;
    };
    std::vector<Row> rows;
    {
        Db::Stmt select = db.Prepare(
            "SELECT d.session_id, d.kind, d.payload, k.wrapped FROM documents_v6 d"
            " JOIN session_keys k ON k.session_id = d.session_id WHERE d.seq = 0");
        while (select.Step()) {
            rows.push_back({select.ColumnText(0), select.ColumnText(1), select.ColumnBlob(2),
                            select.ColumnBlob(3)});
        }
    }
    std::map<std::string, ChunkCipher> keys;
    for (const Row& row : rows) {
        auto key = keys.find(row.session);
        if (key == keys.end()) {
            key = keys.emplace(row.session, ChunkCipher::FromWrapped(row.wrapped)).first;
        }
        const ChunkCipher& cipher = key->second;
        const Domain domain = DomainOf(row.kind);
        std::vector<std::uint8_t> plain;
        try {
            plain = cipher.Open(domain, row.session, 0, row.payload);
        } catch (const StoreError&) {
            log::Printf("clinicavt-engine: deleted an unreadable %s document in the upgrade\n",
                        row.kind.c_str());
            Db::Stmt erase =
                db.Prepare("DELETE FROM documents_v6 WHERE session_id = ? AND kind = ?");
            erase.BindText(1, row.session);
            erase.BindText(2, row.kind);
            erase.Step();
            continue;
        }
        const std::int64_t seq = rows::FreshSlotSequence();
        Db::Stmt update = db.Prepare(
            "UPDATE documents_v6 SET seq = ?, payload = ? WHERE session_id = ? AND kind = ?");
        update.BindInt64(1, seq);
        update.BindBlob(2,
                        cipher.Seal(domain, row.session, static_cast<std::uint64_t>(seq), plain));
        update.BindText(3, row.session);
        update.BindText(4, row.kind);
        update.Step();
    }
}

void MigrateFrom6(Db& db) {
    db.Exec("ALTER TABLE turns RENAME TO turns_v6");
    db.Exec("ALTER TABLE documents RENAME TO documents_v6");
    ResealAtSequenceZero(db);
    db.Exec(kSchemaSql);
    db.Exec(kMigrate6To7Sql);
}

struct Step {
    std::int64_t from;
    void (*run)(Db& db);
};

constexpr Step kSteps[] = {{6, MigrateFrom6}};

void RequireSound(Db& db) {
    Db::Stmt dangling = db.Prepare("PRAGMA foreign_key_check");
    if (dangling.Step()) {
        throw StoreError(StoreCode::kSchema, "upgrade left a row without its consultation");
    }
    Db::Stmt integrity = db.Prepare("PRAGMA integrity_check");
    if (!integrity.Step() || integrity.ColumnText(0) != "ok") {
        throw StoreError(StoreCode::kSchema, "upgrade failed the integrity check");
    }
}

// Foreign keys are off during a step, so dropping a table deletes nothing through a cascade.
// The pragma has no effect inside a transaction
void Upgrade(Db& db, std::int64_t version) {
    for (const Step& step : kSteps) {
        if (step.from != version) continue;
        db.Exec("PRAGMA foreign_keys=OFF");
        try {
            Db::Transaction txn(db);
            step.run(db);
            RequireSound(db);
            db.SetUserVersion(step.from + 1);
            txn.Commit();
        } catch (...) {
            db.Exec("PRAGMA foreign_keys=ON");
            throw;
        }
        db.Exec("PRAGMA foreign_keys=ON");
        log::Printf("clinicavt-engine: store upgraded from schema %lld to %lld\n",
                    static_cast<long long>(step.from), static_cast<long long>(step.from + 1));
        version = step.from + 1;
    }
    if (version != kSchemaVersion) {
        throw StoreError(StoreCode::kSchema, "no upgrade from schema " + std::to_string(version));
    }
    db.Exec("PRAGMA incremental_vacuum");
    db.CheckpointTruncate();
}

}  // namespace

Db OpenDatabase(const std::filesystem::path& root) {
    Db db(DatabasePath(root));
    const std::int64_t application_id = db.ApplicationId();
    const std::int64_t version = db.UserVersion();
    if (application_id != 0 && application_id != kApplicationId) {
        throw StoreError(StoreCode::kSchema, "not a ClinicAVT store");
    }
    if (version == 0) {
        if (db.QueryInt64("SELECT count(*) FROM sqlite_master") != 0) {
            throw StoreError(StoreCode::kSchema, "not a ClinicAVT store");
        }
        // auto_vacuum is set at creation. Switching to WAL already wrote the header, so
        // rebuild the empty file to apply it
        db.Exec("PRAGMA auto_vacuum=INCREMENTAL");
        db.Exec("VACUUM");
        Db::Transaction txn(db);
        db.Exec(kSchemaSql);
        db.SetApplicationId(kApplicationId);
        db.SetUserVersion(kSchemaVersion);
        txn.Commit();
    } else if (application_id == 0) {
        throw StoreError(StoreCode::kSchema, "not a ClinicAVT store");
    } else if (version > kSchemaVersion) {
        throw StoreVersionError(true);
    } else if (version < kOldestSchemaVersion) {
        throw StoreVersionError(false);
    } else if (version < kSchemaVersion) {
        Upgrade(db, version);
    }
    return db;
}

}  // namespace clinicavt::store
