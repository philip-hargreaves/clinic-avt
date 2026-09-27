#include "adapters/storage/store_migrations.hpp"

#include "adapters/storage/schema.hpp"
#include "ports/store_error.hpp"

namespace clinicavt::store {
namespace {

std::filesystem::path DatabasePath(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    return root / "clinicavt.db";
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
        // Incremental vacuum is creation-time. The WAL switch already wrote the
        // header, so the empty file is rebuilt to take it
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
        throw StoreError(StoreCode::kSchema, "store schema is newer than this build");
    } else if (version < kSchemaVersion) {
        throw StoreError(StoreCode::kSchema, "store schema is older than this build");
    }
    return db;
}

}  // namespace clinicavt::store
