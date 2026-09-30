#include "core/archive/backup.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "adapters/archive/archive_file.hpp"
#include "adapters/archive/archive_record.hpp"
#include "adapters/storage/sqlite_session_store.hpp"
#include "core/common/iso8601.hpp"

namespace clinicavt::archive {
namespace {

using store::AddOutcome;
using store::DocumentKind;
using store::SessionId;
using store::SessionRecord;
using store::SqliteSessionStore;

// The minimum count a reader accepts, which keeps key derivation fast in tests
constexpr std::uint32_t kLowIterations = 1000;
constexpr const char* kPassword = "maple-orbit-fender-quill-harbor";
constexpr auto kNever = std::chrono::hours(1);

struct TempDir {
    std::filesystem::path path;

    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("clinicavt-backup-" +
                std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "-" +
                ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }

    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

// Consultation recorded now, with note, sheet and label, plus an optional reflection
SessionId Consultation(SqliteSessionStore& store, bool reflected) {
    const SessionId id = store.Begin({16000, "usb-1", "Desk microphone"});
    store.ReplaceTurns(id, std::vector<asr::Turn>{{0, 16000, "Doctor", "how is the elbow"},
                                                  {16000, 8000, "Patient", "still swollen"}});
    store.Finalise(id);
    store.SaveDocument(id, DocumentKind::kNote,
                       {.text = "Swollen left elbow.", .style = "soap", .detail = "concise"});
    store.SaveDocument(id, DocumentKind::kPatient, {.text = "Your elbow is swollen."});
    store.SaveDocument(id, DocumentKind::kLabel, {.text = "Elbow swelling"});
    if (reflected) {
        store.SaveDocument(id, DocumentKind::kSummary, {.text = "A patient in their forties."});
        store.SaveDocument(id, DocumentKind::kReflection, {.text = R"({"learned":"look"})"});
    }
    return id;
}

SessionRecord Dated(std::string id, std::string started_at) {
    SessionRecord record;
    record.id = std::move(id);
    record.started_at = started_at;
    record.ended_at = started_at;
    record.sample_rate = 16000;
    record.turns = {{0, 16000, "Doctor", "and the knee"}};
    store::RecordDocument note;
    note.document = {.text = "Knee review.", .generated_at = started_at, .revision = 1};
    record.documents = {note};
    return record;
}

std::vector<SessionId> Sorted(std::vector<SessionId> ids) {
    std::ranges::sort(ids);
    return ids;
}

BackupResult BackUpTo(const std::filesystem::path& path, SqliteSessionStore& store,
                      const Period& period, const Progress& progress = {},
                      bool reflections_only = false) {
    ArchiveFileSink sink(path, kPassword, kLowIterations);
    return BackUp(store, store, period, sink, progress, reflections_only);
}

RestoreResult RestoreFrom(const std::filesystem::path& path, SqliteSessionStore& store,
                          bool dry_run = false) {
    ArchiveFileSource source(path, kPassword);
    return Restore(store, store, source, dry_run, {});
}

// Backs up the period's finished and cleared consultations and skips samples, a crashed recording
// and other periods. Restored into another store they come back whole, and a second restore adds
// nothing
TEST(Backup, APeriodBacksUpWholeAndRestoresOnceIntoAnotherStore) {
    TempDir dir;
    SqliteSessionStore here(dir.path / "here", kNever);
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const Period period{Iso8601(now - std::chrono::hours(1)), ""};

    const SessionId reflected = Consultation(here, true);
    const SessionId plain = Consultation(here, false);
    const SessionId cleared = Consultation(here, true);
    here.Clear(cleared);
    here.Seed({.started_at = Iso8601(now), .ended_at = Iso8601(now), .turns = {{0, 1, "", "x"}}});
    const SessionId crashed = here.Begin({16000, "", ""});
    here.Append(crashed, std::vector<float>(1600, 0.1f), 0);
    here.Abandon(crashed);
    ASSERT_EQ(here.AddRecord(Dated(std::string(32, 'e'), "2025-01-06T09:00:00Z")),
              AddOutcome::kAdded);

    const Counts counts = Summarise(here, period);
    EXPECT_EQ(counts.consultations, 3u);
    EXPECT_EQ(counts.reflections, 2u);
    EXPECT_EQ(counts.unfinished, 1u) << "the crashed recording, which a backup cannot hold";

    const auto path = dir.path / "backup.clinicavt";
    std::vector<Phase> phases;
    const BackupResult backed =
        BackUpTo(path, here, period, [&](Phase phase, std::size_t, std::size_t total) {
            phases.push_back(phase);
            EXPECT_EQ(total, 3u);
        });
    EXPECT_EQ(Sorted(backed.ids), Sorted({reflected, plain, cleared}));
    EXPECT_EQ(backed.reflections, 2u);
    EXPECT_EQ(backed.manifest.consultations, 3u);
    EXPECT_EQ(phases, (std::vector<Phase>{Phase::kWriting, Phase::kWriting, Phase::kWriting,
                                          Phase::kChecking}));
    const std::string at = backed.manifest.created_at;
    EXPECT_EQ(Uncovered(here, period, at), 1u) << "the older consultation is in no backup";
    EXPECT_EQ(Uncovered(here, period, Iso8601(now - std::chrono::hours(2))), 4u)
        << "a backup made before them holds none of them";

    std::map<SessionId, nlohmann::json> originals;
    for (const SessionId& id : backed.ids) originals[id] = ToJson(here.ReadRecord(id));
    for (const SessionId& id : backed.ids) here.Delete(id);  // reflections go too

    SqliteSessionStore there(dir.path / "there", kNever);
    const RestoreResult preview = RestoreFrom(path, there, true);
    EXPECT_EQ(preview.Restored(), 3u);
    EXPECT_EQ(preview.reflections, 2u);
    EXPECT_TRUE(there.ListSessions().empty()) << "a dry run writes nothing";

    const RestoreResult restored = RestoreFrom(path, there);
    EXPECT_EQ(restored.added, 3u);
    EXPECT_EQ(restored.skipped, 0u);
    EXPECT_EQ(restored.reflections, 2u);
    EXPECT_EQ(restored.manifest.created_at, at);
    for (const auto& [id, original] : originals) {
        SCOPED_TRACE(id);
        EXPECT_EQ(ToJson(there.ReadRecord(id)), original);
    }

    const RestoreResult again = RestoreFrom(path, there);
    EXPECT_EQ(again.Restored(), 0u);
    EXPECT_EQ(again.skipped, 3u);
}

// The reflection written after clearing survives the restore
TEST(Backup, AClearedConsultationIsCompletedByARestore) {
    TempDir dir;
    SqliteSessionStore store(dir.path / "store", kNever);
    const SessionId id = Consultation(store, true);
    const store::Document note = store.ReadDocument(id, DocumentKind::kNote);
    const auto path = dir.path / "backup.clinicavt";
    BackUpTo(path, store, {});
    store.Clear(id);
    store.EditDocument(id, DocumentKind::kReflection, R"({"learned":"look twice"})");
    ASSERT_TRUE(store.ListSessions()[0].cleared);

    const RestoreResult preview = RestoreFrom(path, store, true);
    EXPECT_EQ(preview.Restored(), 1u);
    EXPECT_EQ(preview.skipped, 0u);
    const RestoreResult restored = RestoreFrom(path, store);
    EXPECT_EQ(restored.completed, 1u);
    EXPECT_EQ(restored.reflections, 1u);
    EXPECT_FALSE(store.ListSessions()[0].cleared);
    EXPECT_EQ(store.ReadTurns(id).size(), 2u);
    const store::Document back = store.ReadDocument(id, DocumentKind::kNote);
    EXPECT_EQ(back.text, "Swollen left elbow.");
    EXPECT_EQ(back.style, "soap");
    EXPECT_EQ(back.revision, note.revision)
        << "the note's revision survives, so its guidance is not stale";
    EXPECT_EQ(store.ReadDocument(id, DocumentKind::kPatient).text, "Your elbow is swollen.");
    EXPECT_EQ(store.ReadDocument(id, DocumentKind::kReflection).text,
              R"({"learned":"look twice"})");
    EXPECT_EQ(RestoreFrom(path, store).skipped, 1u) << "whole again, so left alone";
}

// An authenticated file with an invalid second record writes nothing, not even the first record
TEST(Backup, ARecordTheStoreWouldRefuseStopsRestoreBeforeAnyWrite) {
    TempDir dir;
    const auto path = dir.path / "backup.clinicavt";
    {
        ArchiveFileSink sink(path, kPassword, kLowIterations);
        sink.Begin({.consultations = 2});
        sink.Add(Dated(std::string(32, 'a'), "2026-03-09T14:20:00Z"));
        sink.Add(Dated(std::string(32, 'b'), "2026-03-09 14:20:00"));
        sink.Commit();
    }
    SqliteSessionStore store(dir.path / "store", kNever);
    try {
        RestoreFrom(path, store);
        ADD_FAILURE() << "restored a record the store would refuse";
    } catch (const ArchiveError& e) {
        EXPECT_EQ(e.Code(), ArchiveCode::kDamaged);
    }
    EXPECT_TRUE(store.ListSessions().empty());
}

// A record Restore would refuse fails the backup and leaves no file. The store holds a start time
// in a form no build writes
TEST(Backup, ARecordRestoreWouldRefuseFailsTheBackup) {
    TempDir dir;
    SqliteSessionStore here(dir.path / "here", kNever);
    Consultation(here, true);
    store::SessionMeta meta{16000, "", ""};
    meta.started_at = "2026-03-09 14:20:00";
    here.Finalise(here.Begin(meta));

    const auto path = dir.path / "backup.clinicavt";
    try {
        BackUpTo(path, here, {});
        ADD_FAILURE() << "backed up a record Restore would refuse";
    } catch (const ArchiveError& e) {
        EXPECT_EQ(e.Code(), ArchiveCode::kDamaged);
    }
    EXPECT_FALSE(std::filesystem::exists(path));
}

// Backs up only consultations with an appraisal entry, each as a cleared one with no transcript,
// note, sheet or device. Restoring a later full backup completes them
TEST(Backup, ReflectionsOnlyHoldsNothingOfTheConsultationAndAFullRestoreCompletesIt) {
    TempDir dir;
    SqliteSessionStore here(dir.path / "here", kNever);
    const SessionId reflected = Consultation(here, true);
    Consultation(here, false);

    const auto path = dir.path / "reflections.clinicavt";
    const BackupResult backed = BackUpTo(path, here, {}, {}, true);
    EXPECT_TRUE(backed.ids.empty()) << "it backs up no consultation";
    EXPECT_EQ(backed.manifest.consultations, 1u);
    EXPECT_EQ(backed.reflections, 1u);
    EXPECT_TRUE(backed.manifest.reflections_only);

    {
        ArchiveFileSource source(path, kPassword);
        EXPECT_TRUE(source.GetManifest().reflections_only);
        const auto record = source.Next();
        ASSERT_TRUE(record.has_value());
        EXPECT_EQ(record->id, reflected);
        EXPECT_TRUE(record->turns.empty());
        EXPECT_TRUE(record->device_name.empty());
        std::vector<DocumentKind> kinds;
        for (const auto& entry : record->documents) kinds.push_back(entry.kind);
        EXPECT_EQ(kinds, (std::vector<DocumentKind>{DocumentKind::kLabel, DocumentKind::kSummary,
                                                    DocumentKind::kReflection}));
    }

    SqliteSessionStore there(dir.path / "there", kNever);
    const RestoreResult restored = RestoreFrom(path, there);
    EXPECT_EQ(restored.added, 1u);
    EXPECT_EQ(restored.reflections, 1u);
    ASSERT_EQ(there.ListSessions().size(), 1u);
    EXPECT_TRUE(there.ListSessions()[0].cleared);
    EXPECT_TRUE(there.ReadTurns(reflected).empty());

    const auto full = dir.path / "full.clinicavt";
    BackUpTo(full, here, {});
    const RestoreResult completed = RestoreFrom(full, there);
    EXPECT_EQ(completed.completed, 1u);
    EXPECT_EQ(completed.added, 1u) << "the consultation without a reflection";
    EXPECT_EQ(there.ReadTurns(reflected).size(), 2u);
    EXPECT_EQ(there.ReadDocument(reflected, DocumentKind::kNote).text, "Swollen left elbow.");
}

TEST(Backup, AReflectionsOnlyFileWithAConsultationInItIsDamaged) {
    TempDir dir;
    const auto path = dir.path / "backup.clinicavt";
    {
        ArchiveFileSink sink(path, kPassword, kLowIterations);
        sink.Begin({.consultations = 1, .reflections_only = true});
        sink.Add(Dated(std::string(32, 'a'), "2026-03-09T14:20:00Z"));
        sink.Commit();
    }
    SqliteSessionStore store(dir.path / "store", kNever);
    try {
        RestoreFrom(path, store, true);
        ADD_FAILURE() << "accepted a consultation in a reflections-only file";
    } catch (const ArchiveError& e) {
        EXPECT_EQ(e.Code(), ArchiveCode::kDamaged);
    }
    EXPECT_TRUE(store.ListSessions().empty());
}

}  // namespace
}  // namespace clinicavt::archive
