#include "adapters/storage/db.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace clinicavt::store {
namespace {

struct TempDb {
    std::filesystem::path path;

    TempDb() {
        path =
            std::filesystem::temp_directory_path() /
            ("clinicavt-db-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
             "-" + ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".db");
    }

    ~TempDb() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(path.string() + "-wal", ignored);
        std::filesystem::remove(path.string() + "-shm", ignored);
    }
};

TEST(Db, ASessionDatabaseOpensDurableAndSecureDeleting) {
    TempDb temp;
    Db db(temp.path);
    EXPECT_EQ(db.QueryInt64("PRAGMA page_size"), 8192);
    EXPECT_EQ(db.QueryInt64("PRAGMA synchronous"), 2) << "FULL";
    EXPECT_EQ(db.QueryInt64("PRAGMA foreign_keys"), 1);
    EXPECT_EQ(db.QueryInt64("PRAGMA secure_delete"), 2) << "FAST: freed cells are zeroed";
    EXPECT_EQ(db.QueryInt64("PRAGMA busy_timeout"), 5000) << "a second connection waits";
    Db::Stmt journal = db.Prepare("PRAGMA journal_mode");
    ASSERT_TRUE(journal.Step());
    EXPECT_EQ(journal.ColumnText(0), "wal");
}

TEST(Db, ATransactionRollsBackUnlessCommitted) {
    TempDb temp;
    {
        Db db(temp.path);
        db.Exec("CREATE TABLE t(seq INTEGER PRIMARY KEY)");
        {
            Db::Transaction txn(db);
            db.Exec("INSERT INTO t(seq) VALUES(1)");
        }
        EXPECT_EQ(db.QueryInt64("SELECT COUNT(*) FROM t"), 0) << "no commit, no row";
        Db::Transaction txn(db);
        db.Exec("INSERT INTO t(seq) VALUES(2)");
        txn.Commit();
    }
    Db reopened(temp.path);
    EXPECT_EQ(reopened.QueryInt64("SELECT seq FROM t"), 2) << "the commit survives reopen";
}

TEST(Db, AFailedOpenOrStatementThrowsACodedErrorAndReleasesTheFile) {
    TempDb temp;
    std::ofstream(temp.path, std::ios::binary) << std::string(200, 'x');
    EXPECT_THROW(Db{temp.path}, std::runtime_error);
    EXPECT_TRUE(std::filesystem::remove(temp.path)) << "the handle was closed on the throw";

    Db db(temp.path);
    EXPECT_THROW(db.Exec("NOT ACTUAL SQL"), StoreError);
    try {
        (void)db.Prepare("SELECT * FROM missing");
        FAIL() << "a missing table prepared";
    } catch (const StoreError& e) {
        EXPECT_EQ(e.Code(), StoreCode::kSchema);
    }
}

}  // namespace
}  // namespace clinicavt::store
