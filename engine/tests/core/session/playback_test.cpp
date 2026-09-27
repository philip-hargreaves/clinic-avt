#include "core/session/playback.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "adapters/storage/sqlite_session_store.hpp"

namespace clinicavt::session {
namespace {

using audio::LevelReading;
using audio::SourceEndReason;

using store::DocumentKind;

// Everything the events see, in order, as "kind:payload"
struct RecordingEvents : ISessionEvents {
    std::mutex mutex;
    std::vector<std::string> log;
    std::vector<double> seconds;

    void Add(std::string line) {
        std::lock_guard<std::mutex> lock(mutex);
        log.push_back(std::move(line));
    }
    void OnLevel(const LevelReading&) override {
        Add("level");
    }
    void OnPlaybackLevel(const LevelReading& reading, double at) override {
        std::lock_guard<std::mutex> lock(mutex);
        seconds.push_back(at);
        EXPECT_GT(reading.level, 0.0F);
    }
    void OnInterrupted(SourceEndReason, const std::string&) override {
        Add("interrupted");
    }
    void OnProgress(const std::string& stage) override {
        Add("progress:" + stage);
    }
    void OnNotePartial(const std::string& text) override {
        Add("note-partial:" + text);
    }
    void OnNoteReady(const std::string& text) override {
        Add("note-ready:" + text);
    }
    void OnPatientPartial(const std::string& text) override {
        Add("patient-partial:" + text);
    }
    void OnPatientReady(const std::string& text) override {
        Add("patient-ready:" + text);
    }

    std::vector<std::string> Lines() {
        std::lock_guard<std::mutex> lock(mutex);
        return log;
    }
};

struct Fixture {
    std::filesystem::path root;
    std::unique_ptr<store::SqliteSessionStore> store;
    RecordingEvents events;
    std::vector<std::string> finalised;
    std::vector<std::string> guidance;
    PlaybackPacing fast{.listen = std::chrono::milliseconds(20),
                        .tick = std::chrono::milliseconds(2),
                        .transcript = {},
                        .speakers = {},
                        .guidance = {},
                        .first_token = {},
                        .words_per_second = 500};

    Fixture() {
        root = std::filesystem::temp_directory_path() /
               ("clinicavt-playback-" +
                std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "-" +
                ::testing::UnitTest::GetInstance()->current_test_info()->name());
        store = std::make_unique<store::SqliteSessionStore>(root, std::chrono::hours(1));
    }

    ~Fixture() {
        store.reset();
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    Playback::Hooks Hooks() {
        return {.finalised = [this](const std::string& id) { finalised.push_back(id); },
                .guidance = [this](const std::string& id) { guidance.push_back(id); }};
    }

    // A sealed consultation with every document a real run leaves
    store::SessionId Source() {
        const auto id = store->Begin({16000, "", ""});
        const std::vector<asr::Turn> turns = {
            {0, 32000, "doctor", "Good morning."},
            {40000, 96000, "patient", "My elbow has been swollen."}};
        store->ReplaceTurns(id, turns);
        store->Finalise(id);
        store->SaveDocument(id, DocumentKind::kLabel, {.text = "Elbow swelling"});
        store->SaveDocument(id, DocumentKind::kNote,
                            {.text = "Presented with a swollen left elbow.\n\nPlan: bloods.",
                             .style = "soap",
                             .detail = "concise"});
        store->SaveDocument(id, DocumentKind::kPatient, {.text = "You came in about your elbow."});
        store->SaveDocument(id, DocumentKind::kGuidance,
                            {.text = R"({"version":1,"noteRevision":7,"results":[]})"});
        return id;
    }

    // Listening for ten seconds, so a test acts while the clock runs
    PlaybackPacing Slow() const {
        PlaybackPacing slow = fast;
        slow.listen = std::chrono::seconds(10);
        slow.tick = std::chrono::milliseconds(5);
        return slow;
    }

    static void Settle(Playback& playback) {
        for (int i = 0; i < 500 && playback.Active(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        EXPECT_FALSE(playback.Active());
    }

    std::size_t Readings() {
        std::lock_guard<std::mutex> lock(events.mutex);
        return events.seconds.size();
    }

    // Sleeps are coarse on Windows, so the clock is awaited rather than timed
    bool AwaitClock(const std::function<bool(const std::vector<double>&)>& reached) {
        for (int i = 0; i < 2500; ++i) {
            {
                std::lock_guard<std::mutex> lock(events.mutex);
                if (reached(events.seconds)) return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }
};

TEST(Playback, PlaysTheStoredConsultationBackAsADemoCopy) {
    Fixture f;
    const auto source = f.Source();
    Playback playback(f.events, *f.store, f.Hooks(), f.fast);

    ASSERT_TRUE(playback.Start(source));
    const auto copy = playback.Current();
    EXPECT_NE(copy, source);
    EXPECT_TRUE(playback.Listening());
    ASSERT_TRUE(f.AwaitClock([](const auto& s) { return !s.empty() && s.back() >= 8.5; }))
        << "the clock never reached the end";
    EXPECT_TRUE(playback.Listening()) << "the clock waits for stop at the end";
    playback.Stop();
    EXPECT_FALSE(playback.Listening());
    Fixture::Settle(playback);

    // The clock ran to the end of the audio, 136000 frames
    ASSERT_FALSE(f.events.seconds.empty());
    EXPECT_NEAR(f.events.seconds.back(), 8.5, 1e-9);
    EXPECT_LT(f.events.seconds.front(), f.events.seconds.back());

    const auto lines = f.events.Lines();
    ASSERT_GE(lines.size(), 5u);
    EXPECT_EQ(lines[0], "progress:transcript");
    EXPECT_EQ(lines[1], "progress:speakers");
    EXPECT_TRUE(lines[2].starts_with("note-partial:")) << lines[2];
    std::string last_partial;
    bool note_ready = false;
    for (const auto& line : lines) {
        if (line.starts_with("note-partial:")) {
            const auto text = line.substr(13);
            EXPECT_TRUE(text.starts_with(last_partial)) << text;
            last_partial = text;
        }
        if (line.starts_with("note-ready:")) note_ready = true;
    }
    EXPECT_TRUE(note_ready);
    EXPECT_EQ(lines.back(), "patient-ready:You came in about your elbow.");
    EXPECT_EQ(f.finalised, std::vector<std::string>{copy});
    EXPECT_EQ(f.guidance, std::vector<std::string>{copy});

    // The copy is a demo record with the source's transcript and documents
    const auto sessions = f.store->ListSessions();
    ASSERT_EQ(sessions.size(), 2u);
    for (const auto& session : sessions) {
        EXPECT_EQ(session.demo, session.id == copy) << session.id;
        EXPECT_EQ(session.label, "Elbow swelling");
    }
    EXPECT_EQ(f.store->ReadTurns(copy).size(), 2u);
    const auto note = f.store->ReadDocument(copy, DocumentKind::kNote);
    EXPECT_EQ(note.text, "Presented with a swollen left elbow.\n\nPlan: bloods.");
    EXPECT_EQ(note.style, "soap");
    EXPECT_EQ(note.detail, "concise");
    EXPECT_EQ(f.store->ReadDocument(copy, DocumentKind::kPatient).text,
              "You came in about your elbow.");
    EXPECT_EQ(f.store->ReadDocument(copy, DocumentKind::kGuidance).text, "")
        << "the source's search is not copied, the copy's note is searched afresh";
}

TEST(Playback, PauseHoldsTheClockAndAnEarlyStopFinalisesFromThere) {
    Fixture f;
    Playback playback(f.events, *f.store, f.Hooks(), f.Slow());

    ASSERT_TRUE(playback.Start(f.Source()));
    ASSERT_TRUE(f.AwaitClock([](const auto& s) { return !s.empty(); }));
    playback.SetPaused(true);
    // Nothing is awaited here: the test is that nothing happens
    std::this_thread::sleep_for(std::chrono::milliseconds(20));  // a tick in flight lands
    const auto held = f.Readings();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_EQ(f.Readings(), held) << "a paused clock holds";
    playback.SetPaused(false);
    ASSERT_TRUE(f.AwaitClock([held](const auto& s) { return s.size() > held; }))
        << "and runs on when resumed";

    playback.Stop();
    Fixture::Settle(playback);
    EXPECT_LT(f.events.seconds.back(), 8.5) << "finalised from where the clock was";
    EXPECT_EQ(f.events.Lines().front(), "progress:transcript");
    EXPECT_EQ(f.finalised.size(), 1u);
}

TEST(Playback, RefusesWhatItCannotPlayAndCancelLeavesNoCopy) {
    Fixture f;
    Playback playback(f.events, *f.store, f.Hooks(), f.Slow());

    EXPECT_FALSE(playback.Start("no-such-session"));
    const auto bare = f.store->Begin({16000, "", ""});
    const std::vector<asr::Turn> hello = {{0, 16000, "", "hello"}};
    f.store->ReplaceTurns(bare, hello);
    f.store->Finalise(bare);
    EXPECT_FALSE(playback.Start(bare)) << "no note to show";
    EXPECT_EQ(f.store->ListSessions().size(), 1u) << "no copy left behind";

    const auto source = f.Source();
    ASSERT_TRUE(playback.Start(source));
    EXPECT_FALSE(playback.Start(source)) << "one at a time";
    EXPECT_EQ(f.store->ListSessions().size(), 3u) << "the copy exists while listening";
    playback.Cancel();

    EXPECT_FALSE(playback.Active());
    EXPECT_EQ(f.store->ListSessions().size(), 2u) << "a cancel erases the copy";
    EXPECT_TRUE(f.events.Lines().empty());
    EXPECT_TRUE(f.finalised.empty());
}

}  // namespace
}  // namespace clinicavt::session
