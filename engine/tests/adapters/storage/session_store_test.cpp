#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "adapters/storage/chunk_cipher.hpp"
#include "adapters/storage/db.hpp"
#include "adapters/storage/sqlite_session_store.hpp"

namespace clinicavt::store {
namespace {

using namespace std::chrono_literals;

// Long enough that the writer thread never fires by itself, so a test that
// wants deterministic chunking gets exactly one chunk from Finalise, Abandon or close
constexpr auto kNever = std::chrono::hours(1);

struct TempRoot {
    std::filesystem::path path;

    TempRoot() {
        path = std::filesystem::temp_directory_path() /
               ("clinicavt-store-" +
                std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "-" +
                ::testing::UnitTest::GetInstance()->current_test_info()->name());
    }

    ~TempRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    std::filesystem::path DbPath() const {
        return path / "clinicavt.db";
    }

    std::filesystem::path WalPath() const {
        return path / "clinicavt.db-wal";
    }
};

// Polls until the condition holds, false after five seconds
template <typename Condition>
bool WaitUntil(Condition condition) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

std::int64_t ChunkCount(const TempRoot& root, const SessionId& id) {
    Db db(root.DbPath());
    Db::Stmt count = db.Prepare("SELECT COUNT(*) FROM chunks WHERE session_id = ?");
    count.BindText(1, id);
    count.Step();
    return count.ColumnInt64(0);
}

ChunkCipher CipherOf(const TempRoot& root, const SessionId& id) {
    Db db(root.DbPath());
    Db::Stmt key = db.Prepare("SELECT wrapped FROM session_keys WHERE session_id = ?");
    key.BindText(1, id);
    if (!key.Step()) throw std::runtime_error("no key row for " + id);
    return ChunkCipher::FromWrapped(key.ColumnBlob(0));
}

std::vector<float> Ramp(std::size_t frames) {
    std::vector<float> audio(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        audio[i] = static_cast<float>(i % 1000) / 1000.0f - 0.5f;
    }
    return audio;
}

std::span<const std::uint8_t> Bytes(const std::string& text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

std::vector<std::uint8_t> ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool Holds(const std::vector<std::uint8_t>& bytes, std::span<const std::uint8_t> needle) {
    return std::search(bytes.begin(), bytes.end(), needle.begin(), needle.end()) != bytes.end();
}

bool FileHolds(const std::filesystem::path& path, std::span<const std::uint8_t> needle) {
    return Holds(ReadFileBytes(path), needle);
}

struct StoredChunk {
    std::int64_t first_frame;
    std::int64_t lost_before;
    std::vector<float> frames;
};

// Reads a session the way recovery will: key row, then chunks
std::vector<StoredChunk> DecryptSession(const TempRoot& root, const SessionId& id) {
    const ChunkCipher cipher = CipherOf(root, id);
    Db db(root.DbPath());
    Db::Stmt select = db.Prepare(
        "SELECT seq, first_frame, lost_before, payload FROM chunks WHERE session_id = ?"
        " ORDER BY seq");
    select.BindText(1, id);
    std::vector<StoredChunk> chunks;
    std::int64_t expected_seq = 0;
    while (select.Step()) {
        EXPECT_EQ(select.ColumnInt64(0), expected_seq);
        const std::vector<std::uint8_t> plain = cipher.Open(
            Domain::kAudio, id, static_cast<std::uint64_t>(expected_seq), select.ColumnBlob(3));
        StoredChunk chunk{select.ColumnInt64(1), select.ColumnInt64(2),
                          std::vector<float>(plain.size() / sizeof(float))};
        std::memcpy(chunk.frames.data(), plain.data(), plain.size());
        chunks.push_back(std::move(chunk));
        ++expected_seq;
    }
    return chunks;
}

std::vector<float> Joined(const std::vector<StoredChunk>& chunks) {
    std::vector<float> joined;
    for (const auto& chunk : chunks)
        joined.insert(joined.end(), chunk.frames.begin(), chunk.frames.end());
    return joined;
}

std::vector<SessionId> Ids(const std::vector<SessionSummary>& sessions) {
    std::vector<SessionId> ids;
    for (const auto& session : sessions) ids.push_back(session.id);
    return ids;
}

// Still marked recording: what a crash left, or the live session
std::vector<SessionId> Recording(const std::vector<SessionSummary>& sessions) {
    std::vector<SessionId> ids;
    for (const auto& session : sessions) {
        if (session.state == "recording") ids.push_back(session.id);
    }
    return ids;
}

TEST(SessionStore, ARecordedConsultationSealsItsTranscriptAndErasesItsAudio) {
    TempRoot root;
    const auto audio = Ramp(40000);  // 2.5 s at 16 kHz
    const std::vector<asr::Turn> attributed = {
        {0, 16000, "doctor", "how long have you had the pain"},
        {16000, 24000, "patient", "about three weeks"}};
    SessionId id;
    {
        SqliteSessionStore store(root.path, 30ms);
        id = store.Begin({16000, "mic-1", "Test microphone"});
        store.Append(id, std::span(audio).first(15000), 320);
        ASSERT_TRUE(WaitUntil([&] { return ChunkCount(root, id) > 0; }))
            << "the writer commits while recording";
        store.Append(id, std::span(audio).subspan(15000), 0);
        store.ReplaceTurns(id, std::vector<asr::Turn>{{0, 20000, "", "first one"},
                                                      {20000, 20000, "", "first two"}});
        store.ReplaceTurns(id, attributed);
        store.Finalise(id);

        EXPECT_TRUE(store.ReadAudio(id).empty()) << "the seal erases the recording";
        const auto turns = store.ReadTurns(id);
        ASSERT_EQ(turns.size(), attributed.size()) << "the second write replaces the first";
        for (std::size_t i = 0; i < turns.size(); ++i) {
            SCOPED_TRACE("turn " + std::to_string(i));
            EXPECT_EQ(turns[i].first_frame, attributed[i].first_frame);
            EXPECT_EQ(turns[i].frame_count, attributed[i].frame_count);
            EXPECT_EQ(turns[i].speaker, attributed[i].speaker);
            EXPECT_EQ(turns[i].text, attributed[i].text);
        }
    }

    Db db(root.DbPath());
    EXPECT_EQ(db.QueryInt64("SELECT COUNT(*) FROM chunks"), 0) << "committed and pending alike";
    // Numbering continues past the replaced turns, so no sequence (IV) is sealed twice
    Db::Stmt seqs = db.Prepare("SELECT seq FROM turns WHERE session_id = ? ORDER BY seq");
    seqs.BindText(1, id);
    std::vector<std::int64_t> sequences;
    while (seqs.Step()) sequences.push_back(seqs.ColumnInt64(0));
    EXPECT_EQ(sequences, (std::vector<std::int64_t>{2, 3}));

    Db::Stmt row = db.Prepare(
        "SELECT state, ended_at IS NOT NULL, sample_rate, device_name, lost_frames FROM sessions"
        " WHERE id = ?");
    row.BindText(1, id);
    ASSERT_TRUE(row.Step());
    EXPECT_EQ(row.ColumnText(0), "finalised");
    EXPECT_EQ(row.ColumnInt64(1), 1);
    EXPECT_EQ(row.ColumnInt64(2), 16000);
    EXPECT_EQ(row.ColumnText(3), "Test microphone");
    EXPECT_EQ(row.ColumnInt64(4), 320) << "the audio goes but the loss count stays";
}

TEST(SessionStore, ACrashedOrAbandonedSessionStaysRecoverable) {
    TempRoot root;
    const auto crashed_audio = Ramp(16000 * 3);
    SessionId crashed;
    {
        SqliteSessionStore store(root.path, kNever);
        crashed = store.Begin({16000, "", ""});
        store.Append(crashed, crashed_audio, 0);
    }  // gone mid-recording with no turns sealed. The close commits what was buffered

    SqliteSessionStore store(root.path, kNever);
    {
        const auto listed = store.ListSessions();
        ASSERT_EQ(listed.size(), 1u);
        EXPECT_EQ(listed[0].state, "recording");
        EXPECT_TRUE(listed[0].ended_at.empty());
        EXPECT_NEAR(listed[0].audio_seconds, 3.0, 0.01) << "the length comes from the chunks";
        EXPECT_EQ(store.ReadAudio(crashed), crashed_audio);
    }

    const auto abandoned_audio = Ramp(8000);
    const SessionId abandoned = store.Begin({16000, "", ""});
    store.Append(abandoned, abandoned_audio, 320);
    store.ReplaceTurns(abandoned, std::vector<asr::Turn>{{0, 8000, "", "kept for recovery"}});
    store.Abandon(abandoned);
    const auto turns = store.ReadTurns(abandoned);
    ASSERT_EQ(turns.size(), 1u);
    EXPECT_EQ(turns[0].text, "kept for recovery");
    const auto chunks = DecryptSession(root, abandoned);
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0].lost_before, 320) << "the chunk carries the loss before it";
    EXPECT_EQ(chunks[0].frames, abandoned_audio);

    // The store is free for the next session
    const SessionId live = store.Begin({16000, "", ""});
    EXPECT_EQ(Ids(store.ListSessions()), (std::vector<SessionId>{live, abandoned, crashed}))
        << "newest first";

    store.Finalise(live);
    EXPECT_EQ(Recording(store.ListSessions()), (std::vector<SessionId>{abandoned, crashed}))
        << "a finalised session is not recoverable";
    const auto listed = store.ListSessions();
    EXPECT_EQ(listed[0].state, "finalised");
    EXPECT_FALSE(listed[0].ended_at.empty());
}

TEST(SessionStore, CancelErasesAtOnceAndTheStoreRecordsAgain) {
    TempRoot root;
    const auto audio = Ramp(16000);
    SqliteSessionStore store(root.path, 30ms);
    const SessionId cancelled = store.Begin({16000, "", ""});
    store.Append(cancelled, audio, 0);
    store.ReplaceTurns(cancelled, std::vector<asr::Turn>{{0, 16000, "", "never kept"}});
    ASSERT_TRUE(WaitUntil([&] { return ChunkCount(root, cancelled) > 0; }));
    store.Cancel(cancelled);

    EXPECT_TRUE(store.ListSessions().empty());
    EXPECT_THROW((void)store.ReadAudio(cancelled), std::runtime_error) << "the key is gone";
    {
        Db db(root.DbPath());
        for (const char* table : {"sessions", "session_keys", "chunks", "turns"}) {
            EXPECT_EQ(db.QueryInt64((std::string("SELECT COUNT(*) FROM ") + table).c_str()), 0)
                << table;
        }
    }

    const SessionId next = store.Begin({16000, "", ""});
    store.Append(next, audio, 0);
    store.Abandon(next);
    EXPECT_EQ(store.ReadAudio(next), audio);
}

TEST(SessionStore, TheLiveSessionAndUnknownIdsAreRefused) {
    TempRoot root;
    SqliteSessionStore store(root.path, kNever);
    const SessionId stored = store.Begin({16000, "", ""});
    store.Finalise(stored);
    const SessionId live = store.Begin({16000, "", ""});
    const SessionId unknown = "nope";
    const std::vector<asr::Turn> turns = {{0, 16000, "doctor", "late"}};

    struct Refusal {
        const char* call;
        StoreCode code;
        std::function<void()> run;
    };
    const std::vector<Refusal> refusals = {
        {"a second Begin", StoreCode::kBusy, [&] { (void)store.Begin({16000, "", ""}); }},
        {"ReadTurns of the live session", StoreCode::kBusy, [&] { (void)store.ReadTurns(live); }},
        {"ReadAudio of the live session", StoreCode::kBusy, [&] { (void)store.ReadAudio(live); }},
        {"Delete of the live session", StoreCode::kBusy, [&] { store.Delete(live); }},
        {"SaveDocument on the live session", StoreCode::kBusy,
         [&] { store.SaveDocument(live, DocumentKind::kNote, {.text = "early"}); }},
        {"EditDocument on the live session", StoreCode::kBusy,
         [&] { store.EditDocument(live, DocumentKind::kNote, "early"); }},
        {"ReadDocument of the live session", StoreCode::kBusy,
         [&] { (void)store.ReadDocument(live, DocumentKind::kNote); }},
        {"ReplaceTurns on a sealed session", StoreCode::kNotFound,
         [&] { store.ReplaceTurns(stored, turns); }},
        {"Append to an unknown id", StoreCode::kNotFound,
         [&] { store.Append(unknown, Ramp(100), 0); }},
        {"Finalise of an unknown id", StoreCode::kNotFound, [&] { store.Finalise(unknown); }},
        {"Cancel of an unknown id", StoreCode::kNotFound, [&] { store.Cancel(unknown); }},
        {"ReadTurns of an unknown id", StoreCode::kNotFound,
         [&] { (void)store.ReadTurns(unknown); }},
        {"Delete of an unknown id", StoreCode::kNotFound, [&] { store.Delete(unknown); }},
        {"ReadDocument of an unknown id", StoreCode::kNotFound,
         [&] { (void)store.ReadDocument(unknown, DocumentKind::kNote); }},
        {"DeleteDocument of an unknown id", StoreCode::kNotFound,
         [&] { store.DeleteDocument(unknown, DocumentKind::kReflection); }},
    };
    for (const Refusal& refusal : refusals) {
        SCOPED_TRACE(refusal.call);
        try {
            refusal.run();
            ADD_FAILURE() << "not refused";
        } catch (const StoreError& e) {
            EXPECT_EQ(e.Code(), refusal.code);
        }
    }

    store.Finalise(live);
    EXPECT_THROW(store.Append(live, Ramp(100), 0), std::runtime_error) << "Append after Finalise";
}

TEST(SessionStore, EachDocumentKindKeepsItsOwnSlotThroughSaveEditAndDelete) {
    TempRoot root;
    SqliteSessionStore store(root.path, kNever);
    const SessionId id = store.Begin({16000, "", ""});
    store.Finalise(id);
    EXPECT_EQ(store.ReadDocument(id, DocumentKind::kNote).text, "") << "absent until saved";

    // The note: a generation, the clinician's edit over it, then a regeneration
    store.SaveDocument(id, DocumentKind::kNote,
                       {.text = "generated", .style = "soap", .detail = "concise"});
    Document note = store.ReadDocument(id, DocumentKind::kNote);
    EXPECT_EQ(note.text, "generated");
    EXPECT_EQ(note.language, "en");
    EXPECT_EQ(note.style, "soap");
    EXPECT_EQ(note.detail, "concise");
    EXPECT_FALSE(note.generated_at.empty());
    EXPECT_TRUE(note.edited_at.empty());
    const std::string generated_at = note.generated_at;

    store.EditDocument(id, DocumentKind::kNote, "the clinician's wording");
    note = store.ReadDocument(id, DocumentKind::kNote);
    EXPECT_EQ(note.text, "the clinician's wording");
    EXPECT_EQ(note.style, "soap") << "an edit keeps the options";
    EXPECT_EQ(note.detail, "concise");
    EXPECT_EQ(note.generated_at, generated_at);
    EXPECT_FALSE(note.edited_at.empty());
    EXPECT_FALSE(store.ListSessions()[0].edited_at.empty()) << "a note edit edits the record";

    store.SaveDocument(id, DocumentKind::kNote, {.text = "regenerated", .style = "prose"});
    note = store.ReadDocument(id, DocumentKind::kNote);
    EXPECT_EQ(note.text, "regenerated");
    EXPECT_EQ(note.style, "prose") << "a generation replaces the options";
    EXPECT_TRUE(note.edited_at.empty()) << "and the edit";

    // Every other kind in its own slot. A kind/check/domain mismatch fails its row
    struct Slot {
        const char* name;
        DocumentKind kind;
        std::string text;
        std::string language;
    };
    const std::vector<Slot> slots = {
        {"patient", DocumentKind::kPatient, "Your elbow is swollen.", "en"},
        {"translation", DocumentKind::kTranslation, "Twój łokieć", "pl"},
        {"label", DocumentKind::kLabel, "Elbow swelling", "en"},
        {"summary", DocumentKind::kSummary, "A patient in their forties.", "en"},
        {"reflection", DocumentKind::kReflection, R"({"happened":"x"})", "en"},
        {"guidance", DocumentKind::kGuidance, R"({"version":1})", "en"},
    };
    for (const Slot& slot : slots) {
        store.SaveDocument(id, slot.kind, {.text = slot.text, .language = slot.language});
    }
    for (const Slot& slot : slots) {
        SCOPED_TRACE(slot.name);
        const Document document = store.ReadDocument(id, slot.kind);
        EXPECT_EQ(document.text, slot.text);
        EXPECT_EQ(document.language, slot.language);
    }
    EXPECT_EQ(store.ReadDocument(id, DocumentKind::kNote).text, "regenerated");

    auto listed = store.ListSessions();
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_EQ(listed[0].label, "Elbow swelling");
    EXPECT_TRUE(listed[0].has_reflection);
    store.EditDocument(id, DocumentKind::kReflection, R"({"happened":"x2"})");
    EXPECT_FALSE(store.ReadDocument(id, DocumentKind::kReflection).edited_at.empty());
    EXPECT_TRUE(store.ListSessions()[0].edited_at.empty())
        << "a reflection is not an edit to the clinical record";

    store.DeleteDocument(id, DocumentKind::kReflection);
    EXPECT_EQ(store.ReadDocument(id, DocumentKind::kReflection).text, "");
    EXPECT_TRUE(store.ListSessions()[0].has_reflection) << "the summary alone is an entry";
    EXPECT_EQ(store.ReadDocument(id, DocumentKind::kSummary).text, "A patient in their forties.");
    store.DeleteDocument(id, DocumentKind::kSummary);
    EXPECT_FALSE(store.ListSessions()[0].has_reflection);
    EXPECT_NO_THROW(store.DeleteDocument(id, DocumentKind::kReflection)) << "gone already";
}

TEST(SessionStore, RewritingADocumentNeverReusesANonce) {
    TempRoot root;
    SqliteSessionStore store(root.path, kNever);
    const SessionId id = store.Begin({16000, "", ""});
    store.Finalise(id);
    const Document note{.text = "the note", .style = "prose", .detail = "standard"};

    std::vector<std::vector<std::uint8_t>> payloads;
    std::vector<std::int64_t> sequences;
    auto record = [&] {
        Db db(root.DbPath());
        Db::Stmt row = db.Prepare("SELECT seq, payload FROM documents WHERE kind = 'note'");
        ASSERT_TRUE(row.Step());
        sequences.push_back(row.ColumnInt64(0));
        payloads.push_back(row.ColumnBlob(1));
        EXPECT_EQ(store.ReadDocument(id, DocumentKind::kNote).text, "the note");
    };
    store.SaveDocument(id, DocumentKind::kNote, note);
    record();
    store.EditDocument(id, DocumentKind::kNote, "the note");
    record();
    store.SaveDocument(id, DocumentKind::kNote, note);
    record();

    EXPECT_EQ(sequences, (std::vector<std::int64_t>{1, 2, 3}));
    EXPECT_EQ(store.ReadDocument(id, DocumentKind::kNote).revision, 3);
    EXPECT_EQ(store.ReadDocument(id, DocumentKind::kPatient).revision, 0) << "absent";
    EXPECT_NE(payloads[0], payloads[1]) << "the same text, a different IV";
    EXPECT_NE(payloads[1], payloads[2]);
    EXPECT_NE(payloads[0], payloads[2]);
}

// While the store is open its writes sit in the WAL; closing folds them into the file.
// Both files are scanned at both points
TEST(SessionStore, NoContentIsPlaintextAtRest) {
    TempRoot root;
    const std::vector<std::string> sentinels = {
        "SENTINEL-HYPERTENSION-PHRASE", "SENTINEL-BURSITIS-PHRASE", "SENTINEL-PATIENT-PHRASE",
        R"({"trigger":"SENTINEL-GUIDANCE-PHRASE"})"};
    const auto audio = Ramp(32000);
    const auto* window = reinterpret_cast<const std::uint8_t*>(audio.data() + 1000);
    const std::span<const std::uint8_t> audio_window(window, 64 * sizeof(float));

    auto expect_no_plaintext = [&](const char* when) {
        SCOPED_TRACE(when);
        for (const auto& file : {root.DbPath(), root.WalPath()}) {
            const auto bytes = ReadFileBytes(file);
            for (const std::string& sentinel : sentinels) {
                EXPECT_FALSE(Holds(bytes, Bytes(sentinel)))
                    << file.filename().string() << " holds " << sentinel;
            }
            EXPECT_FALSE(Holds(bytes, audio_window)) << file.filename().string() << " holds audio";
        }
    };

    {
        SqliteSessionStore store(root.path, kNever);
        const SessionId finalised = store.Begin({16000, "", ""});
        store.ReplaceTurns(finalised, std::vector<asr::Turn>{{0, 16000, "doctor", sentinels[0]}});
        store.Finalise(finalised);
        store.SaveDocument(finalised, DocumentKind::kNote, {.text = sentinels[1]});
        store.SaveDocument(finalised, DocumentKind::kPatient, {.text = sentinels[2]});
        store.SaveDocument(finalised, DocumentKind::kGuidance, {.text = sentinels[3]});
        const SessionId abandoned = store.Begin({16000, "", ""});
        store.Append(abandoned, audio, 0);
        store.Abandon(abandoned);  // recoverable, so the audio is on disk
        ASSERT_EQ(store.ReadAudio(abandoned), audio);

        // The scan must see the stored ciphertext, or finding nothing proves nothing
        std::vector<std::uint8_t> sealed_turn;
        {
            Db db(root.DbPath());
            Db::Stmt row = db.Prepare("SELECT payload FROM turns");
            ASSERT_TRUE(row.Step());
            sealed_turn = row.ColumnBlob(0);
        }
        ASSERT_TRUE(std::filesystem::exists(root.WalPath()));
        ASSERT_TRUE(FileHolds(root.DbPath(), sealed_turn) ||
                    FileHolds(root.WalPath(), sealed_turn));
        expect_no_plaintext("store open");
    }
    expect_no_plaintext("store closed");
}

TEST(SessionStore, DeleteErasesTheSessionItsKeyAndEveryRowButNeverTheLiveOne) {
    TempRoot root;
    SqliteSessionStore store(root.path, kNever);
    const SessionId finished = store.Begin({16000, "", ""});
    store.ReplaceTurns(finished, std::vector<asr::Turn>{{0, 16000, "", "to be erased"}});
    store.Finalise(finished);
    store.SaveDocument(finished, DocumentKind::kNote, {.text = "a note", .style = "soap"});
    store.SaveDocument(finished, DocumentKind::kGuidance, {.text = "{}"});
    const SessionId crashed = store.Begin({16000, "", ""});
    store.Append(crashed, Ramp(16000), 0);
    store.ReplaceTurns(crashed, std::vector<asr::Turn>{{0, 16000, "", "to be erased"}});
    store.Abandon(crashed);
    ASSERT_EQ(ChunkCount(root, crashed), 1);

    store.Delete(finished);
    store.Delete(crashed);

    EXPECT_TRUE(store.ListSessions().empty());
    EXPECT_THROW((void)store.ReadTurns(finished), std::runtime_error);
    {
        Db db(root.DbPath());
        for (const char* table :
             {"sessions", "session_keys", "turns", "chunks", "documents", "note_options"}) {
            EXPECT_EQ(db.QueryInt64((std::string("SELECT COUNT(*) FROM ") + table).c_str()), 0)
                << table;
        }
    }

    // Delete all spares the session still recording
    const SessionId stored = store.Begin({16000, "", ""});
    store.Finalise(stored);
    store.SaveDocument(stored, DocumentKind::kNote, {.text = "a note"});
    SessionSeed seed;
    seed.started_at = "2026-03-09T14:20:00Z";
    seed.ended_at = "2026-03-09T14:31:51Z";
    store.Seed(seed);
    const SessionId live = store.Begin({16000, "", ""});

    EXPECT_EQ(store.DeleteAll(), 2u);

    EXPECT_THROW((void)store.ReadDocument(stored, DocumentKind::kNote), std::runtime_error);
    store.Finalise(live);
    EXPECT_EQ(Ids(store.ListSessions()), (std::vector<SessionId>{live}));
    EXPECT_EQ(store.DeleteAll(), 1u);
    EXPECT_TRUE(store.ListSessions().empty());
}

TEST(SessionStore, ErasedKeysLeaveNoRemnantInTheFileOrWal) {
    TempRoot root;
    SqliteSessionStore store(root.path, kNever);
    const SessionId id = store.Begin({16000, "", ""});
    store.Finalise(id);
    std::vector<std::uint8_t> wrapped;
    {
        Db db(root.DbPath());
        Db::Stmt key = db.Prepare("SELECT wrapped FROM session_keys WHERE session_id = ?");
        key.BindText(1, id);
        ASSERT_TRUE(key.Step());
        wrapped = key.ColumnBlob(0);
    }
    ASSERT_GT(wrapped.size(), 96u);
    // Past the header every DPAPI blob shares
    const std::vector<std::uint8_t> needle(wrapped.begin() + 32, wrapped.begin() + 96);
    ASSERT_TRUE(FileHolds(root.DbPath(), needle) || FileHolds(root.WalPath(), needle));

    store.Delete(id);
    EXPECT_FALSE(FileHolds(root.DbPath(), needle));
    EXPECT_FALSE(FileHolds(root.WalPath(), needle));
}

TEST(SessionStore, RetainOffSessionsAreHiddenThenSweptButACrashedOneWaitsForRecovery) {
    TempRoot root;
    const auto audio = Ramp(16000);
    SessionMeta drop{16000, "", ""};
    drop.retain = false;
    SqliteSessionStore store(root.path, kNever);

    const SessionId crashed = store.Begin(drop);
    store.Append(crashed, audio, 0);
    store.Abandon(crashed);
    const SessionId kept = store.Begin({16000, "", ""});
    store.ReplaceTurns(kept, std::vector<asr::Turn>{{0, 16000, "doctor", "kept"}});
    store.Finalise(kept);
    const SessionId dropped = store.Begin(drop);
    store.ReplaceTurns(dropped, std::vector<asr::Turn>{{0, 16000, "doctor", "dropped"}});
    store.Finalise(dropped);
    // Readable by id until the consultation is left, but never in history
    ASSERT_EQ(store.ReadTurns(dropped).size(), 1u);
    EXPECT_EQ(Ids(store.ListSessions()), (std::vector<SessionId>{kept, crashed}));

    store.EraseUnretained();

    EXPECT_EQ(Ids(store.ListSessions()), (std::vector<SessionId>{kept, crashed}));
    EXPECT_THROW((void)store.ReadTurns(dropped), std::runtime_error);
    {
        Db db(root.DbPath());
        EXPECT_EQ(db.QueryInt64("SELECT COUNT(*) FROM session_keys"), 2) << "its key is gone";
        EXPECT_EQ(db.QueryInt64("SELECT COUNT(*) FROM turns"), 1);
    }
    EXPECT_EQ(Recording(store.ListSessions()), (std::vector<SessionId>{crashed}))
        << "the crashed one's audio is still needed";
    EXPECT_EQ(store.ReadAudio(crashed), audio);
}

TEST(SessionStore, AFreshStoreIsStampedAndForeignOlderOrNewerFilesAreRefused) {
    TempRoot root;
    {
        SqliteSessionStore store(root.path, kNever);
        store.Finalise(store.Begin({16000, "", ""}));
    }
    {
        Db db(root.DbPath());
        EXPECT_EQ(db.UserVersion(), 6);
        EXPECT_EQ(db.ApplicationId(), 0x414D4243) << "AMBC";
        EXPECT_EQ(db.QueryInt64("PRAGMA auto_vacuum"), 2) << "incremental";
        EXPECT_EQ(db.QueryInt64("PRAGMA foreign_keys"), 1);
        db.SetUserVersion(999);
    }
    EXPECT_THROW(SqliteSessionStore(root.path, kNever), std::runtime_error)
        << "a store from a newer build";
    {
        Db db(root.DbPath());
        db.SetUserVersion(5);
    }
    EXPECT_THROW(SqliteSessionStore(root.path, kNever), std::runtime_error)
        << "a store from an older build: there are no migrations";
    {
        Db db(root.DbPath());
        db.SetUserVersion(6);
        db.SetApplicationId(0);
    }
    EXPECT_THROW(SqliteSessionStore(root.path, kNever), std::runtime_error)
        << "a store without the mark";

    const auto foreign = root.path / "foreign";
    std::filesystem::create_directories(foreign);
    {
        Db db(foreign / "clinicavt.db");
        db.Exec("CREATE TABLE notes(text TEXT)");
    }
    EXPECT_THROW(SqliteSessionStore(foreign, kNever), std::runtime_error);
    {
        Db db(foreign / "clinicavt.db");
        EXPECT_EQ(db.QueryInt64("SELECT count(*) FROM sqlite_master WHERE name = 'sessions'"), 0)
            << "no schema was created into it";
        // Another application's mark, whatever the version says
        db.Exec("DROP TABLE notes");
        db.SetApplicationId(0x11111111);
        db.SetUserVersion(5);
    }
    EXPECT_THROW(SqliteSessionStore(foreign, kNever), std::runtime_error);
}

// A page cap at the file's current size makes every commit fail as a full disk would. The
// capture is held, past the bound the oldest second is dropped, and the stored timeline moves
// with it so it stays aligned with the turns
TEST(SessionStore, AFullDiskHoldsTheAudioThenDropsTheOldestAndKeepsTheTimeline) {
    TempRoot root;
    SqliteSessionStore store(root.path, 25ms);
    std::mutex mutex;
    std::vector<StoreCode> faults;
    store.SetFaultListener([&](const StoreError& fault) {
        std::lock_guard<std::mutex> lock(mutex);
        faults.push_back(fault.Code());
    });
    const SessionId id = store.Begin({16000, "", ""});
    {
        Db db(root.DbPath());
        store.SetMaxPageCount(db.QueryInt64("PRAGMA page_count"));
    }

    const auto audio = Ramp(16000 * 31);  // one second over the bound
    store.Append(id, std::span(audio).first(16000), 0);
    ASSERT_TRUE(WaitUntil([&] {
        std::lock_guard<std::mutex> lock(mutex);
        return !faults.empty();
    }));
    store.Append(id, std::span(audio).subspan(16000), 0);
    std::this_thread::sleep_for(150ms);  // several failing ticks: one trims the oldest second
    {
        std::lock_guard<std::mutex> lock(mutex);
        ASSERT_EQ(faults.size(), 1u) << "announced once per episode";
        EXPECT_EQ(faults[0], StoreCode::kFull);
    }

    store.SetMaxPageCount(0);
    store.Abandon(id);
    const auto chunks = DecryptSession(root, id);
    ASSERT_FALSE(chunks.empty());
    EXPECT_EQ(chunks[0].first_frame, 16000) << "the timeline skips the dropped second";
    EXPECT_EQ(chunks[0].lost_before, 16000);
    EXPECT_EQ(Joined(chunks), std::vector<float>(audio.begin() + 16000, audio.end()))
        << "held while the disk was full, stored once it was not";
}

// Listing unwraps one key per labelled session, so a long list holds the database lock.
// The capture thread's append must not queue behind it
TEST(SessionStore, AppendNeverWaitsOnTheDatabase) {
    TempRoot root;
    SqliteSessionStore store(root.path, kNever);
    for (int i = 0; i < 300; ++i) {
        const SessionId seeded =
            store.Seed({"2026-01-01T00:00:00Z", "2026-01-01T00:10:00Z", 16000, {}});
        store.SaveDocument(seeded, DocumentKind::kLabel, {.text = "Elbow swelling"});
    }
    const SessionId id = store.Begin({16000, "", ""});
    std::atomic<bool> listing{true};
    std::thread lister([&] {
        while (listing) (void)store.ListSessions();
    });

    const auto frame = Ramp(160);
    std::chrono::steady_clock::duration worst{0};
    for (int i = 0; i < 200; ++i) {
        const auto started = std::chrono::steady_clock::now();
        store.Append(id, frame, 0);
        worst = std::max(worst, std::chrono::steady_clock::now() - started);
        std::this_thread::sleep_for(1ms);
    }
    listing = false;
    lister.join();
    EXPECT_LT(worst, 10ms) << "an append waited on the store";
    store.Abandon(id);
    EXPECT_EQ(Joined(DecryptSession(root, id)).size(), 200u * 160u);
}

}  // namespace
}  // namespace clinicavt::store
