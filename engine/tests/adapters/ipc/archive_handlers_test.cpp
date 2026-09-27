#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "adapters/archive/archive_lane.hpp"
#include "adapters/ipc/handlers.hpp"
#include "adapters/storage/sqlite_session_store.hpp"
#include "core/common/utf8.hpp"

namespace clinicavt::ipc {
namespace {

using archive::ArchiveLane;
using store::DocumentKind;

constexpr std::uint32_t kLowIterations = 1000;

json LoadFixture(const std::string& name) {
    std::ifstream in(std::string(CLINICAVT_FIXTURE_DIR) + "/" + name);
    if (!in.is_open()) throw std::runtime_error("missing fixture: " + name);
    return json::parse(in);
}

const json& ResultOf(const std::variant<json, Error>& outcome) {
    return std::get<json>(outcome);
}

void ExpectSameKeys(const json& actual, const json& fixture) {
    for (const auto& [key, value] : fixture.items()) EXPECT_TRUE(actual.contains(key)) << key;
    EXPECT_EQ(actual.size(), fixture.size()) << actual.dump();
}

struct Store {
    std::filesystem::path root;
    std::unique_ptr<store::SqliteSessionStore> sessions;

    Store() {
        root = std::filesystem::temp_directory_path() /
               ("clinicavt-archive-handlers-" +
                std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "-" +
                ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(root);
        sessions =
            std::make_unique<store::SqliteSessionStore>(root / "store", std::chrono::hours(1));
    }

    ~Store() {
        sessions.reset();
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    std::string BackupPath() const {
        return utf8::FromPath(root / "backup.clinicavt");
    }

    // A finished consultation with its note, and an appraisal entry when asked
    void Add(const std::string& id, bool reflected) const {
        store::SessionRecord record;
        record.id = id;
        record.started_at = "2026-08-17T10:15:00Z";
        record.ended_at = "2026-08-17T10:23:41Z";
        record.sample_rate = 16000;
        record.turns = {{0, 16000, "Doctor", "how is the elbow"}};
        store::RecordDocument note;
        note.document = {.text = "Swollen elbow.", .revision = 1};
        record.documents = {note};
        if (reflected) {
            store::RecordDocument reflection;
            reflection.kind = DocumentKind::kReflection;
            reflection.document = {.text = R"({"learned":"look"})", .revision = 1};
            record.documents.push_back(reflection);
        }
        if (sessions->AddRecord(record) != store::AddOutcome::kAdded) {
            throw std::runtime_error("not added");
        }
    }
};

// What the lane sends, waited on from the test thread
struct Notices {
    std::mutex mutex;
    std::condition_variable arrived;
    std::vector<std::pair<std::string, json>> seen;

    ArchiveLane::Emit Emit() {
        return [this](const std::string& method, json params) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                seen.emplace_back(method, std::move(params));
            }
            arrived.notify_all();
        };
    }

    // The job's end, done or failed, with the progress before it
    std::pair<std::string, json> Final(std::vector<json>* progress = nullptr) {
        std::unique_lock<std::mutex> lock(mutex);
        const bool ended = arrived.wait_for(lock, std::chrono::seconds(60), [this] {
            return !seen.empty() && seen.back().first != "archive/progress";
        });
        if (!ended) throw std::runtime_error("the job never ended");
        for (const auto& [method, params] : seen) {
            if (progress != nullptr && method == "archive/progress") progress->push_back(params);
        }
        auto last = seen.back();
        seen.clear();
        return last;
    }
};

TEST(ArchiveHandlers, NotificationsMatchTheFixtures) {
    const json backup = LoadFixture("archive-done-backup.json");
    archive::BackupResult backed;
    for (const auto& id : backup["params"]["ids"]) backed.ids.push_back(id.get<std::string>());
    backed.reflections = 12;
    backed.manifest = {.created_at = "2026-09-30T17:42:10Z",
                       .from = "2026-07-01T00:00:00Z",
                       .to = "2026-10-01T00:00:00Z",
                       .consultations = 38};
    EXPECT_EQ(archive::BackupDoneJson(backed), backup["params"]);

    archive::RestoreResult restored{
        .added = 30, .completed = 3, .skipped = 5, .reflections = 10, .manifest = backed.manifest};
    EXPECT_EQ(archive::RestoreDoneJson(restored, true),
              LoadFixture("archive-done-restore.json")["params"]);
    EXPECT_EQ(archive::ArchiveProgressJson("backup", archive::Phase::kWriting, 12, 38),
              LoadFixture("archive-progress.json")["params"]);
    EXPECT_EQ(archive::ArchiveFailedJson("restore", "wrong-password"),
              LoadFixture("archive-failed.json")["params"]);
}

// The fixtures' requests, pointed at a temporary file: a checked backup, a wrong password that
// leaves the store as it was, then a dry run that finds the consultation already here
TEST(ArchiveHandlers, BackUpAndRestoreRunOnTheLaneFromTheFixtureRequests) {
    Store store;
    store.Add("a1b2c3d4e5f60718293a4b5c6d7e8f90", true);
    Notices notices;
    ArchiveLane lane(*store.sessions, notices.Emit(), kLowIterations);

    const json summary = LoadFixture("archive-summary.json");
    json asked = summary["request"]["params"];
    const auto counted = HandleArchiveSummary(*store.sessions, asked);
    ASSERT_TRUE(std::holds_alternative<json>(counted));
    ExpectSameKeys(ResultOf(counted), summary["response"]["result"]);
    EXPECT_EQ(ResultOf(counted)["consultations"], 1);
    EXPECT_EQ(ResultOf(counted)["reflections"], 1);
    EXPECT_EQ(ResultOf(counted)["uncovered"], 0) << "the last backup's period holds it";
    asked["covered"]["to"] = "2026-08-01T00:00:00Z";
    EXPECT_EQ(ResultOf(HandleArchiveSummary(*store.sessions, asked))["uncovered"], 1);
    asked.erase("covered");
    EXPECT_EQ(ResultOf(HandleArchiveSummary(*store.sessions, asked))["uncovered"], 0);

    const json backup = LoadFixture("archive-backup.json");
    json params = backup["request"]["params"];
    params["path"] = store.BackupPath();
    params["from"] = "";  // the shell's open start
    const auto started = HandleArchiveBackup(lane, false, params);
    ASSERT_TRUE(std::holds_alternative<json>(started));
    EXPECT_EQ(MakeResult(std::int64_t{32}, ResultOf(started)), backup["response"]);
    std::vector<json> progress;
    auto end = notices.Final(&progress);
    ASSERT_EQ(end.first, "archive/done") << end.second.dump();
    ExpectSameKeys(end.second, LoadFixture("archive-done-backup.json")["params"]);
    EXPECT_EQ(end.second["consultations"], 1);
    EXPECT_EQ(end.second["ids"], json::array({"a1b2c3d4e5f60718293a4b5c6d7e8f90"}));
    EXPECT_EQ(end.second["from"], "");
    EXPECT_EQ(end.second["to"], "2026-10-01T00:00:00Z");
    ASSERT_FALSE(progress.empty());
    EXPECT_EQ(progress.front()["phase"], "writing");
    EXPECT_EQ(progress.back()["phase"], "checking");

    const json restore = LoadFixture("archive-restore.json");
    params = restore["request"]["params"];
    params["path"] = store.BackupPath();
    params["password"] = "maple-orbit-fender-quill-harbour";
    ASSERT_TRUE(std::holds_alternative<json>(HandleArchiveRestore(lane, false, params)));
    end = notices.Final();
    EXPECT_EQ(end.first, "archive/failed");
    EXPECT_EQ(end.second, LoadFixture("archive-failed.json")["params"]);
    EXPECT_EQ(store.sessions->ListSessions().size(), 1u);

    params = restore["request"]["params"];
    params["path"] = store.BackupPath();
    const auto checked = HandleArchiveRestore(lane, false, params);
    ASSERT_TRUE(std::holds_alternative<json>(checked));
    EXPECT_EQ(MakeResult(std::int64_t{33}, ResultOf(checked)), restore["response"]);
    end = notices.Final();
    ASSERT_EQ(end.first, "archive/done") << end.second.dump();
    ExpectSameKeys(end.second, LoadFixture("archive-done-restore.json")["params"]);
    EXPECT_EQ(end.second["dryRun"], true);
    EXPECT_EQ(end.second["consultations"], 0);
    EXPECT_EQ(end.second["skipped"], 1);
}

// Nothing starts during a consultation or beside another job, deletes are refused while one
// runs, and a malformed request never reaches the lane
TEST(ArchiveHandlers, JobsAndDeletesAreRefusedWhileBusyAndBadParamsNeverStartOne) {
    Store store;
    store.Add("a1b2c3d4e5f60718293a4b5c6d7e8f90", false);
    Notices notices;
    ArchiveLane lane(*store.sessions, notices.Emit());  // the real count, so the job lasts
    const json backup{{"from", ""},
                      {"to", ""},
                      {"path", store.BackupPath()},
                      {"password", "maple-orbit-fender-quill-harbor"}};
    const json restore{{"path", store.BackupPath()},
                       {"password", "maple-orbit-fender-quill-harbor"},
                       {"dryRun", true}};
    const json remove{{"ids", json::array({"a1b2c3d4e5f60718293a4b5c6d7e8f90"})}};

    auto refused = [](const std::variant<json, Error>& outcome, int code) {
        return std::holds_alternative<Error>(outcome) && std::get<Error>(outcome).code == code;
    };
    EXPECT_TRUE(refused(HandleArchiveBackup(lane, true, backup), kSessionError));
    EXPECT_TRUE(refused(HandleArchiveRestore(lane, true, restore), kSessionError));
    EXPECT_FALSE(lane.Busy());

    ASSERT_TRUE(std::holds_alternative<json>(HandleArchiveBackup(lane, false, backup)));
    const bool busy = lane.Busy();  // deriving the key alone outlasts these calls
    ASSERT_TRUE(busy);
    EXPECT_TRUE(refused(HandleArchiveBackup(lane, false, backup), kSessionError));
    EXPECT_TRUE(refused(HandleArchiveRestore(lane, false, restore), kSessionError));
    EXPECT_TRUE(refused(HandleSessionRemove(*store.sessions, remove, busy), kSessionError));
    EXPECT_TRUE(refused(HandleSessionDeleteAll(*store.sessions, json::object(), false, busy),
                        kSessionError));
    EXPECT_EQ(notices.Final().first, "archive/done");
    EXPECT_EQ(store.sessions->ListSessions().size(), 1u);

    struct Case {
        const char* what;
        std::function<std::variant<json, Error>()> call;
    };
    auto without = [](json params, const char* key) {
        params.erase(key);
        return params;
    };
    auto with = [](json params, const char* key, json value) {
        params[key] = std::move(value);
        return params;
    };
    const Case cases[] = {
        {"backup without a path",
         [&] { return HandleArchiveBackup(lane, false, without(backup, "path")); }},
        {"backup to an empty path",
         [&] { return HandleArchiveBackup(lane, false, with(backup, "path", "")); }},
        {"backup from a date not in the store's form",
         [&] { return HandleArchiveBackup(lane, false, with(backup, "from", "2026-07-01")); }},
        {"backup with a numeric password",
         [&] { return HandleArchiveBackup(lane, false, with(backup, "password", 12345678)); }},
        {"restore without a path",
         [&] { return HandleArchiveRestore(lane, false, without(restore, "path")); }},
        {"restore with dryRun as text",
         [&] { return HandleArchiveRestore(lane, false, with(restore, "dryRun", "yes")); }},
        {"summary with a numeric start",
         [&] { return HandleArchiveSummary(*store.sessions, json{{"from", 3}}); }},
        {"summary covered without its time",
         [&] {
             return HandleArchiveSummary(*store.sessions,
                                         json{{"covered", {{"from", ""}, {"to", ""}}}});
         }},
        {"remove without a list",
         [&] { return HandleSessionRemove(*store.sessions, json{{"ids", "a1b2"}}, false); }},
        {"remove with a numeric id",
         [&] {
             return HandleSessionRemove(*store.sessions, json{{"ids", json::array({1})}}, false);
         }},
        {"remove with deleteReflections as text",
         [&] {
             return HandleSessionRemove(*store.sessions, with(remove, "deleteReflections", "no"),
                                        false);
         }},
        {"delete all with deleteReflections as a number",
         [&] {
             return HandleSessionDeleteAll(*store.sessions, json{{"deleteReflections", 1}}, false,
                                           false);
         }},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.what);
        EXPECT_TRUE(refused(c.call(), kInvalidParams));
        EXPECT_FALSE(lane.Busy());
    }
    EXPECT_EQ(store.sessions->ListSessions().size(), 1u);
}

// Remove and Delete all keep each appraisal entry unless asked: the consultation leaves the
// history list and its entry stays in the journal
TEST(ArchiveHandlers, RemoveAndDeleteAllKeepReflectionsUnlessAskedAndMatchTheFixtures) {
    Store store;
    store.Add("a1b2c3d4e5f60718293a4b5c6d7e8f90", true);
    store.Add("0f1e2d3c4b5a69788796a5b4c3d2e1f0", false);

    const json remove = LoadFixture("session-remove.json");
    const auto removed = HandleSessionRemove(*store.sessions, remove["request"]["params"], false);
    ASSERT_TRUE(std::holds_alternative<json>(removed));
    EXPECT_EQ(MakeResult(std::int64_t{34}, ResultOf(removed)), remove["response"]);
    EXPECT_TRUE(HandleSessionList(*store.sessions)["sessions"].empty());
    json journal = HandleReflectionList(*store.sessions)["reflections"];
    ASSERT_EQ(journal.size(), 1u);
    EXPECT_EQ(journal[0]["id"], "a1b2c3d4e5f60718293a4b5c6d7e8f90");

    for (int i = 0; i < 40; ++i) {
        char id[33];
        std::snprintf(id, sizeof id, "%032x", static_cast<unsigned>(i + 1));
        store.Add(id, false);
    }
    const json delete_all = LoadFixture("session-deleteAll.json");
    const auto erased =
        HandleSessionDeleteAll(*store.sessions, delete_all["request"]["params"], false, false);
    ASSERT_TRUE(std::holds_alternative<json>(erased));
    EXPECT_EQ(MakeResult(std::int64_t{35}, ResultOf(erased)), delete_all["response"]);
    EXPECT_EQ(HandleReflectionList(*store.sessions)["reflections"].size(), 1u);

    const auto everything =
        HandleSessionDeleteAll(*store.sessions, json{{"deleteReflections", true}}, false, false);
    ASSERT_TRUE(std::holds_alternative<json>(everything));
    EXPECT_EQ(ResultOf(everything)["removed"], 1);
    EXPECT_TRUE(HandleReflectionList(*store.sessions)["reflections"].empty());
    EXPECT_TRUE(store.sessions->ListSessions().empty());
}

}  // namespace
}  // namespace clinicavt::ipc
