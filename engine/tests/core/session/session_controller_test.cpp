#include "core/session/session_controller.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "adapters/transcription/scripted_transcriber.hpp"
#include "adapters/vad/passthrough_vad.hpp"

namespace clinicavt::session {
namespace {

using audio::EnrolProgress;
using audio::IAudioSink;
using audio::IAudioSource;
using audio::kSampleRate;
using audio::LevelMeter;
using audio::LevelReading;
using audio::PassthroughVad;
using audio::SourceEndReason;

constexpr auto kTestSettle = std::chrono::milliseconds(200);
// Enough audio that each of two merged turns clears the 0.3 s decode floor
constexpr std::size_t kTwoTurnFrames = 12800;

// Sleeps are coarse on Windows, so conditions are polled rather than timed
template <typename Pred>
bool WaitFor(Pred done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

// One 100 ms window at an amplitude the meter reads as full scale
std::vector<float> Window() {
    return std::vector<float>(LevelMeter::kWindowFrames, 0.70710678F);
}

class ScriptedSource : public IAudioSource {
   public:
    enum class Script {
        kStreamUntilStopped,
        kDieImmediately,
        kDieAfterAudio,
        kNeverAudio,
        kThrowAfterAudio,
        kCompleteAfterAudio,
    };

    explicit ScriptedSource(Script script) : script_(script) {}

    void Run(IAudioSink& sink) override {
        const auto window = Window();
        switch (script_) {
            case Script::kDieImmediately:
                sink.OnEnd({SourceEndReason::kFailed, "would not open"});
                return;
            case Script::kNeverAudio:
                while (!stop_.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                sink.OnEnd({SourceEndReason::kStopped, ""});
                return;
            case Script::kDieAfterAudio:
                sink.OnAudio(window, 0);
                sink.OnEnd({SourceEndReason::kDeviceLost, "unplugged"});
                return;
            case Script::kThrowAfterAudio:
                sink.OnAudio(window, 0);
                throw std::runtime_error("driver exploded");
            case Script::kCompleteAfterAudio:
                sink.OnAudio(window, 3);
                sink.OnEnd({SourceEndReason::kCompleted, ""});
                return;
            case Script::kStreamUntilStopped:
                while (!stop_.load()) {
                    if (!paused.load()) sink.OnAudio(window, 0);
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                sink.OnEnd({SourceEndReason::kStopped, ""});
                return;
        }
    }

    void RequestStop() override {
        stop_.store(true);
    }

    void SetPaused(bool p) override {
        paused.store(p);
    }

    void SetMonitor(bool) override {}

    std::atomic<bool> paused{false};

   private:
    Script script_;
    std::atomic<bool> stop_{false};
};

using Script = ScriptedSource::Script;

// Written on the pipeline and lane threads, read under the mutex or after the
// controller has joined them
struct RecordingEvents : ISessionEvents {
    std::mutex mutex;
    std::vector<float> levels;
    std::vector<SourceEndReason> interruptions;
    std::string last_detail;
    std::vector<std::string> progress;
    std::vector<std::string> storage_faults;
    std::vector<EnrolProgress> enrol_progress;
    std::optional<std::tuple<bool, std::string, double>> enrol_done;

    std::vector<std::string> note_partials;
    std::string note_ready;
    std::string note_failed;
    std::string note_refused;
    bool note_refused_overridable = true;
    bool note_done = false;
    std::string note_saved_session;
    std::string note_saved_text;
    std::int64_t note_saved_revision = 0;
    bool note_saved_throws = false;

    std::vector<std::string> patient_partials;
    std::string patient_ready;
    std::string patient_failed;
    bool patient_done = false;

    std::string summary_session;
    std::string summary_text;
    std::string summary_failed;
    bool summary_done = false;

    template <typename F>
    bool WaitUntil(F done) {
        return WaitFor([&] {
            const std::lock_guard<std::mutex> lock(mutex);
            return done();
        });
    }

    bool WaitForNote() {
        return WaitUntil([this] { return note_done; });
    }

    bool WaitForPatient() {
        return WaitUntil([this] { return patient_done; });
    }

    bool WaitForSummary() {
        return WaitUntil([this] { return summary_done; });
    }

    // Before a second write, so its outcome is not read from the first
    void ResetNote() {
        const std::lock_guard<std::mutex> lock(mutex);
        note_done = false;
        note_partials.clear();
        note_ready.clear();
        note_refused.clear();
    }

    void OnLevel(const LevelReading& reading) override {
        const std::lock_guard<std::mutex> lock(mutex);
        levels.push_back(reading.level);
    }

    void OnInterrupted(SourceEndReason reason, const std::string& detail) override {
        const std::lock_guard<std::mutex> lock(mutex);
        interruptions.push_back(reason);
        last_detail = detail;
    }

    void OnProgress(const std::string& stage) override {
        const std::lock_guard<std::mutex> lock(mutex);
        progress.push_back(stage);
    }

    void OnStorageFault(const std::string& detail) override {
        const std::lock_guard<std::mutex> lock(mutex);
        storage_faults.push_back(detail);
    }

    void OnEnrolProgress(const EnrolProgress& p) override {
        const std::lock_guard<std::mutex> lock(mutex);
        enrol_progress.push_back(p);
    }

    void OnEnrolDone(bool ok, const std::string& detail, double speech_s) override {
        const std::lock_guard<std::mutex> lock(mutex);
        enrol_done = {ok, detail, speech_s};
    }

    void OnNotePartial(const std::string& text) override {
        const std::lock_guard<std::mutex> lock(mutex);
        note_partials.push_back(text);
    }

    void OnNoteReady(const std::string& text) override {
        const std::lock_guard<std::mutex> lock(mutex);
        note_ready = text;
        note_done = true;
    }

    void OnNoteSaved(const std::string& session, const store::Document& note) override {
        if (note_saved_throws) throw std::runtime_error("the lane refused");
        const std::lock_guard<std::mutex> lock(mutex);
        note_saved_session = session;
        note_saved_text = note.text;
        note_saved_revision = note.revision;
    }

    void OnNoteFailed(const std::string& detail) override {
        const std::lock_guard<std::mutex> lock(mutex);
        note_failed = detail;
        note_done = true;
    }

    void OnNoteRefused(const std::string& reason, bool overridable) override {
        const std::lock_guard<std::mutex> lock(mutex);
        note_refused = reason;
        note_refused_overridable = overridable;
        note_done = true;
    }

    void OnPatientPartial(const std::string& text) override {
        const std::lock_guard<std::mutex> lock(mutex);
        patient_partials.push_back(text);
    }

    void OnPatientReady(const std::string& text) override {
        const std::lock_guard<std::mutex> lock(mutex);
        patient_ready = text;
        patient_done = true;
    }

    void OnPatientFailed(const std::string& detail) override {
        const std::lock_guard<std::mutex> lock(mutex);
        patient_failed = detail;
        patient_done = true;
    }

    void OnSummaryReady(const std::string& session, const std::string& text) override {
        const std::lock_guard<std::mutex> lock(mutex);
        summary_session = session;
        summary_text = text;
        summary_done = true;
    }

    void OnSummaryFailed(const std::string& session, const std::string& detail) override {
        const std::lock_guard<std::mutex> lock(mutex);
        summary_session = session;
        summary_failed = detail;
        summary_done = true;
    }
};

// Records the call sequence so the tests can assert which storage outcome each
// way of ending a session produced
struct FakeSessionStore : store::ISessionStore {
    std::mutex mutex;
    std::vector<std::string> calls;
    std::vector<float> frames;
    std::vector<asr::Turn> turns;
    std::vector<float> stored_audio;
    std::uint64_t lost = 0;
    int begins = 0;
    int sweeps = 0;
    bool refuse_begin = false;
    bool refuse_read_audio = false;
    bool refuse_read_turns = false;
    bool refuse_documents = false;
    bool fault_audio = false;  // an audio commit fails, as the real store reports it
    bool refuse_outcome = false;
    std::function<void(const store::StoreError&)> on_fault;

    bool last_retain = true;
    std::string last_device_id;
    std::string last_device_name;

    std::string note;
    std::int64_t note_revision = 0;
    std::string note_style;
    std::string note_detail;
    std::string patient;
    std::string summary;
    std::string label;
    bool label_typed = false;

    std::vector<std::string> Calls() {
        const std::lock_guard<std::mutex> lock(mutex);
        return calls;
    }

    void SetFaultListener(std::function<void(const store::StoreError&)> listener) override {
        on_fault = std::move(listener);
    }

    store::SessionId Begin(const store::SessionMeta& meta) override {
        const std::lock_guard<std::mutex> lock(mutex);
        if (refuse_begin) throw std::runtime_error("store is broken");
        EXPECT_EQ(meta.sample_rate, kSampleRate);
        last_retain = meta.retain;
        last_device_id = meta.device_id;
        last_device_name = meta.device_name;
        const auto id = "s" + std::to_string(++begins);
        calls.push_back("begin " + id);
        return id;
    }

    void Append(const store::SessionId&, std::span<const float> audio,
                std::uint64_t lost_frames) override {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            frames.insert(frames.end(), audio.begin(), audio.end());
            lost += lost_frames;
        }
        if (fault_audio && on_fault) {
            on_fault(store::StoreError(store::StoreCode::kFull, "audio commit failed"));
        }
    }

    void ReplaceTurns(const store::SessionId& id, std::span<const asr::Turn> replacement) override {
        const std::lock_guard<std::mutex> lock(mutex);
        calls.push_back("replace " + id);
        turns.assign(replacement.begin(), replacement.end());
    }

    void Outcome(const char* what, const store::SessionId& id) {
        const std::lock_guard<std::mutex> lock(mutex);
        calls.push_back(what + (" " + id));
        if (refuse_outcome) throw store::StoreError(store::StoreCode::kIo, "seal failed");
    }

    void Finalise(const store::SessionId& id) override {
        Outcome("finalise", id);
    }

    void Cancel(const store::SessionId& id) override {
        Outcome("cancel", id);
    }

    void Abandon(const store::SessionId& id) override {
        Outcome("abandon", id);
    }

    std::vector<store::SessionSummary> ListSessions() override {
        return {};
    }

    void SaveDocument(const store::SessionId& id, store::DocumentKind kind,
                      const store::Document& document) override {
        const std::lock_guard<std::mutex> lock(mutex);
        if (refuse_documents) throw store::StoreError(store::StoreCode::kFull, "disk full");
        switch (kind) {
            case store::DocumentKind::kNote:
                calls.push_back("note " + id);
                note = document.text;
                ++note_revision;
                note_style = document.style;
                note_detail = document.detail;
                break;
            case store::DocumentKind::kPatient:
                calls.push_back("patient " + id);
                patient = document.text;
                break;
            case store::DocumentKind::kLabel:
                label = document.text;
                label_typed = false;
                break;
            case store::DocumentKind::kSummary:
                calls.push_back("summary " + id);
                summary = document.text;
                break;
            default:
                break;
        }
    }

    void EditDocument(const store::SessionId&, store::DocumentKind, const std::string&) override {}

    void DeleteDocument(const store::SessionId&, store::DocumentKind) override {}

    store::Document ReadDocument(const store::SessionId&, store::DocumentKind kind) override {
        const std::lock_guard<std::mutex> lock(mutex);
        store::Document document;
        switch (kind) {
            case store::DocumentKind::kNote:
                document.text = note;
                document.revision = note_revision;
                break;
            case store::DocumentKind::kPatient:
                document.text = patient;
                break;
            case store::DocumentKind::kLabel:
                document.text = label;
                document.edited_at = label_typed ? "typed" : "";
                break;
            case store::DocumentKind::kSummary:
                document.text = summary;
                break;
            default:
                break;
        }
        return document;
    }

    std::vector<asr::Turn> ReadTurns(const store::SessionId& id) override {
        const std::lock_guard<std::mutex> lock(mutex);
        if (refuse_read_turns) throw std::runtime_error("no session " + id);
        return turns;
    }

    std::vector<float> ReadAudio(const store::SessionId& id) override {
        const std::lock_guard<std::mutex> lock(mutex);
        if (refuse_read_audio) throw std::runtime_error("no session " + id);
        calls.push_back("readAudio " + id);
        return stored_audio;
    }

    void Delete(const store::SessionId& id) override {
        const std::lock_guard<std::mutex> lock(mutex);
        calls.push_back("delete " + id);
    }

    // Counted apart from calls, so the outcome sequences there stay exact
    void EraseUnretained() override {
        const std::lock_guard<std::mutex> lock(mutex);
        ++sweeps;
    }

    store::SessionId Seed(const store::SessionSeed&) override {
        return "seeded";
    }

    std::size_t ClearDemo() override {
        return 0;
    }

    std::size_t DeleteAll(bool) override {
        return 0;
    }

    void Clear(const store::SessionId&) override {}

    store::SessionRecord ReadRecord(const store::SessionId& id) override {
        throw store::StoreError(store::StoreCode::kNotFound, "no session " + id);
    }

    store::AddOutcome AddRecord(const store::SessionRecord&) override {
        return store::AddOutcome::kSkipped;
    }
};

struct FakeNoteWriter : note::INoteWriter {
    std::string result = "the clinical note";
    std::string label_result = "  \"Elbow swelling.\"  ";  // sanitises to Elbow swelling
    bool fail = false;
    bool patient = false;
    bool fail_patient = false;
    bool fail_summary = false;
    std::atomic<bool> block{false};
    std::atomic<bool> block_label{false};
    std::atomic<bool> cancelled{false};
    std::atomic<int> prepares{0};
    std::atomic<int> prefills{0};
    std::atomic<int> label_calls{0};
    std::string last_prefill_speaker;  // read after the controller joins its threads
    std::mutex mutex;
    std::vector<std::vector<asr::Turn>> calls;
    note::NoteOptions last_options;
    std::string patient_input;
    std::string summary_input;

    void Prepare() override {
        ++prepares;
    }

    void Prefill(const std::vector<asr::Turn>& guess, const note::NoteOptions&) override {
        ++prefills;
        if (!guess.empty()) last_prefill_speaker = guess.front().speaker;
    }

    std::string Write(const std::vector<asr::Turn>& transcript, const note::NoteOptions& options,
                      const Progress& progress) override {
        cancelled = false;  // per generation, like the real writer
        {
            const std::lock_guard<std::mutex> lock(mutex);
            calls.push_back(transcript);
            last_options = options;
        }
        // Partials are prefixes of the final text, as the real writer streams them
        progress(result.substr(0, std::min<std::size_t>(11, result.size())));
        progress(result.substr(0, std::min<std::size_t>(20, result.size())));
        while (block.load() && !cancelled.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (fail) throw std::runtime_error("generation failed");
        return cancelled.load() ? "interrupted" : result;
    }

    void Cancel() override {
        cancelled = true;
    }

    bool WritesPatient() const override {
        return patient;
    }

    std::string WritePatient(const std::string& note_text, const Progress& progress) override {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            patient_input = note_text;
        }
        progress("Your appointment");
        if (fail_patient) throw std::runtime_error("patient generation failed");
        return "the patient sheet";
    }

    std::string WriteLabel(const std::string&) override {
        ++label_calls;
        while (block_label.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return label_result;
    }

    std::string WriteSummary(const std::string& note_text) override {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            summary_input = note_text;
        }
        if (fail_summary) throw std::runtime_error("summary generation failed");
        return "A patient in their forties presented with a swollen elbow.";
    }
};

// One cluster or two split at half the audio. Counters the tests poll are
// atomic, the rest is read after the controller joins the thread writing it
struct FakeDiariser : diar::IDiariser {
    int clusters = 1;
    std::vector<double> similarities;
    diar::DiariseTiming timing;
    bool fail_diarise = false;
    bool speculate_first_turn = false;
    bool speculate_transcript = false;

    int calls = 0;
    std::size_t audio_frames = 0;
    int similarity_calls = 0;
    int voiceprint_cluster = -1;
    std::atomic<int> accruals{0};
    int accrued_cluster = -1;
    std::atomic<int> advances{0};
    std::size_t advanced_frames = 0;
    std::vector<std::uint64_t> cut_points;
    int settles = 0;
    std::size_t settled_frames = 0;
    int takes = 0;
    int discards = 0;

    std::vector<float> voiceprint{0.6f, 0.8f};  // what EmbedVoice answers
    std::size_t embedded_frames = 0;
    std::vector<float> replaced;
    std::uint64_t replaced_at = 0;

    diar::DiariseResult Diarise(std::span<const float> audio) override {
        ++calls;
        if (fail_diarise) throw std::runtime_error("diariser crashed");
        audio_frames = audio.size();
        diar::DiariseResult result;
        result.cluster_count = clusters;
        result.timing = timing;
        if (clusters == 1) {
            result.slices = {{0, audio.size(), 0}};
        } else {
            const auto half = audio.size() / 2;
            result.slices = {{0, half, 0}, {half, audio.size(), 1}};
        }
        return result;
    }

    std::vector<double> AnchorSimilarities(std::span<const float>,
                                           const std::vector<diar::LabelledSlice>&, int) override {
        ++similarity_calls;
        return similarities;
    }

    std::vector<float> DoctorVoiceprint(std::span<const float>,
                                        const std::vector<diar::LabelledSlice>&,
                                        int doctor_cluster) override {
        voiceprint_cluster = doctor_cluster;
        return {1.0f};
    }

    void AccrueVoiceprint(std::span<const float>) override {
        accrued_cluster = voiceprint_cluster;
        ++accruals;
    }

    void Advance(std::span<const float> audio, const diar::DecodeClipFn&) override {
        advanced_frames = audio.size();
        ++advances;
    }

    void AddCutPoints(std::span<const std::uint64_t> cuts) override {
        cut_points.insert(cut_points.end(), cuts.begin(), cuts.end());
    }

    void Settle(std::span<const float> audio, const diar::DecodeClipFn&) override {
        ++settles;
        settled_frames = audio.size();
    }

    std::vector<asr::Turn> SpeculativeTranscript() override {
        if (!speculate_transcript) return {};
        return {{0, 16000, "doctor", "settled words"}};
    }

    // Pretends capture speculated the first cluster's turn (its span is the
    // first half of the audio Diarise saw)
    diar::TurnTexts TakeTurnTexts() override {
        ++takes;
        diar::TurnTexts cache;
        if (speculate_first_turn && audio_frames > 0) {
            cache[{0, audio_frames / 2}] = "speculated words";
        }
        return cache;
    }

    void DiscardCapture() override {
        ++discards;
    }

    std::vector<float> EmbedVoice(std::span<const float> audio) override {
        embedded_frames = audio.size();
        return voiceprint;
    }

    void ReplaceAnchor(std::span<const float> vp, std::uint64_t at) override {
        replaced.assign(vp.begin(), vp.end());
        replaced_at = at;
    }
};

SourceFactory FactoryFor(Script script) {
    return [script](const std::optional<ReplaySpec>&, const std::string&) {
        return std::make_unique<ScriptedSource>(script);
    };
}

struct RigOptions {
    std::uint64_t advance_frames = 5 * kSampleRate;
    bool writer = false;
    bool metrics = false;
    std::size_t min_note_words = 0;  // most scenarios test the lane, not the thin gate
};

// The controller's collaborators. A controller made here is declared after
// the rig, so it is destroyed first
struct Rig {
    RecordingEvents events;
    FakeSessionStore store;
    asr::ScriptedTranscriber transcriber;
    PassthroughVad vad;
    FakeDiariser diariser;
    FakeNoteWriter writer;
    metrics::Registry registry;

    SessionController Make(SourceFactory factory, const RigOptions& options = {}) {
        return SessionController(std::move(factory), events, store, transcriber, vad, diariser,
                                 kTestSettle, options.advance_frames,
                                 options.writer ? &writer : nullptr,
                                 options.metrics ? &registry : nullptr, options.min_note_words);
    }

    SessionController Make(Script script, const RigOptions& options = {}) {
        return Make(FactoryFor(script), options);
    }

    bool WaitForFrames(std::size_t n) {
        return WaitFor([&] {
            const std::lock_guard<std::mutex> lock(store.mutex);
            return store.frames.size() >= n;
        });
    }
};

// Capture and crash safety

TEST(SessionController, AFailedStartLeavesNoTraceAndTheNextStartWorks) {
    struct Row {
        const char* name;
        Script script;
        bool refuse_begin;
        std::string detail;
        std::vector<std::string> calls;
    };
    const std::vector<Row> rows = {
        {"the source dies first",
         Script::kDieImmediately,
         false,
         "would not open",
         {"begin s1", "cancel s1"}},
        {"no audio before the deadline",
         Script::kNeverAudio,
         false,
         "deadline",
         {"begin s1", "cancel s1"}},
        {"the store refuses the session", Script::kStreamUntilStopped, true, "store", {}},
    };
    for (const auto& row : rows) {
        SCOPED_TRACE(row.name);
        Rig rig;
        rig.store.refuse_begin = row.refuse_begin;
        int sources = 0;
        auto controller = rig.Make([&](const std::optional<ReplaySpec>&, const std::string&) {
            return std::make_unique<ScriptedSource>(sources++ == 0 ? row.script
                                                                   : Script::kStreamUntilStopped);
        });

        EXPECT_FALSE(controller.Start());
        EXPECT_FALSE(controller.Running());
        EXPECT_EQ(controller.LastEnd().reason, SourceEndReason::kFailed);
        EXPECT_NE(controller.LastEnd().detail.find(row.detail), std::string::npos)
            << controller.LastEnd().detail;
        EXPECT_EQ(rig.store.Calls(), row.calls)
            << "a session that never produced audio leaves no trace";
        EXPECT_TRUE(rig.events.interruptions.empty());

        // The failure is not sticky
        rig.store.refuse_begin = false;
        ASSERT_TRUE(controller.Start());
        controller.Stop();
    }
}

TEST(SessionController, EachWayOfEndingASessionHasItsOwnStoreOutcome) {
    enum class Ending { kStop, kCancel, kInterrupted };
    struct Row {
        const char* name;
        Script script;
        Ending ending;
        std::vector<std::string> calls;
        std::optional<SourceEndReason> interruption;
        std::string detail;
        bool store_faults = false;
    };
    const std::vector<Row> rows = {
        {"a stop keeps the attributed recording",
         Script::kStreamUntilStopped,
         Ending::kStop,
         {"begin s1", "replace s1", "finalise s1"}},
        {"a cancel erases it",
         Script::kStreamUntilStopped,
         Ending::kCancel,
         {"begin s1", "cancel s1"}},
        {"a lost device abandons it for recovery",
         Script::kDieAfterAudio,
         Ending::kInterrupted,
         {"begin s1", "abandon s1"},
         SourceEndReason::kDeviceLost,
         "unplugged"},
        {"a throwing source is caught and abandons it",
         Script::kThrowAfterAudio,
         Ending::kInterrupted,
         {"begin s1", "abandon s1"},
         SourceEndReason::kFailed,
         "driver exploded"},
        // One window is under the shortest decodable turn: no transcript
        {"a completed replay waits for the stop",
         Script::kCompleteAfterAudio,
         Ending::kStop,
         {"begin s1", "finalise s1"}},
        {"a completed replay can still be cancelled",
         Script::kCompleteAfterAudio,
         Ending::kCancel,
         {"begin s1", "cancel s1"}},
        {"store faults reach the shell and the session still ends",
         Script::kCompleteAfterAudio,
         Ending::kStop,
         {"begin s1", "finalise s1"},
         std::nullopt,
         "",
         true},
    };
    for (const auto& row : rows) {
        SCOPED_TRACE(row.name);
        Rig rig;
        rig.store.fault_audio = row.store_faults;
        rig.store.refuse_outcome = row.store_faults;
        auto controller = rig.Make(row.script);

        ASSERT_TRUE(controller.Start());
        if (row.script == Script::kStreamUntilStopped) {
            ASSERT_TRUE(rig.WaitForFrames(kTwoTurnFrames));
        }
        if (row.ending == Ending::kCancel) {
            controller.Cancel();
        } else {
            if (row.ending == Ending::kInterrupted) {
                ASSERT_TRUE(
                    rig.events.WaitUntil([&] { return !rig.events.interruptions.empty(); }));
            }
            controller.Stop();
        }

        EXPECT_FALSE(controller.Running());
        EXPECT_EQ(rig.store.Calls(), row.calls);
        if (row.interruption.has_value()) {
            ASSERT_EQ(rig.events.interruptions.size(), 1u);
            EXPECT_EQ(rig.events.interruptions[0], *row.interruption);
            EXPECT_NE(rig.events.last_detail.find(row.detail), std::string::npos);
        } else {
            EXPECT_TRUE(rig.events.interruptions.empty()) << "the user's own ending";
        }
        if (row.ending == Ending::kCancel) {
            EXPECT_EQ(rig.diariser.calls, 0) << "nothing diarises on cancel";
            EXPECT_TRUE(rig.events.progress.empty()) << "a cancel finalises nothing to report";
            EXPECT_EQ(rig.diariser.takes, 0) << "nothing splices on cancel";
            EXPECT_GE(rig.diariser.discards, 1) << "capture state must not leak";
        }
        if (row.script == Script::kCompleteAfterAudio) {
            EXPECT_EQ(rig.store.lost, 3u) << "loss accounting must reach the store";
            EXPECT_EQ(rig.store.frames.size(), Window().size());
        }
        const auto faults = row.store_faults
                                ? std::vector<std::string>{"audio commit failed", "seal failed"}
                                : std::vector<std::string>{};
        EXPECT_EQ(rig.events.storage_faults, faults);
    }
}

TEST(SessionController, OneSessionAtATimeWithLevelsFromTheFirstWindow) {
    Rig rig;
    int sources = 0;
    auto controller = rig.Make([&](const std::optional<ReplaySpec>&, const std::string&) {
        ++sources;
        return std::make_unique<ScriptedSource>(Script::kStreamUntilStopped);
    });

    controller.Stop();
    EXPECT_TRUE(rig.store.Calls().empty()) << "a stop before start is a no-op";

    ASSERT_TRUE(controller.Start()) << "acked once audio flows";
    EXPECT_TRUE(controller.Running());
    EXPECT_FALSE(controller.Start());
    EXPECT_EQ(rig.store.begins, 1) << "the refused start must not open a second session";
    controller.Stop();

    EXPECT_FALSE(controller.Running());
    ASSERT_FALSE(rig.events.levels.empty());
    EXPECT_NEAR(rig.events.levels.front(), 1.0F, 0.01F);
    EXPECT_TRUE(rig.events.interruptions.empty()) << "a user stop is not an interruption";

    ASSERT_TRUE(controller.Start());
    controller.Stop();
    EXPECT_EQ(sources, 2) << "a restart gets a fresh source";
    EXPECT_EQ(rig.store.begins, 2);
}

TEST(SessionController, AResumedSessionContinuesTheStoredAudioAndSupersedesTheOld) {
    Rig rig;
    rig.store.stored_audio = std::vector<float>(LevelMeter::kWindowFrames * 3, 0.5F);
    const auto combined = rig.store.stored_audio.size() + Window().size();
    auto controller = rig.Make(Script::kCompleteAfterAudio);

    rig.store.refuse_read_audio = true;
    EXPECT_FALSE(controller.Start(std::nullopt, "gone")) << "nothing to resume";
    EXPECT_FALSE(controller.Running());
    EXPECT_TRUE(rig.store.Calls().empty());
    rig.store.refuse_read_audio = false;

    ASSERT_TRUE(controller.Start(std::nullopt, "old-session"));
    ASSERT_TRUE(rig.WaitForFrames(combined));
    controller.Stop();
    const auto index = [&](const std::string& call) {
        const auto calls = rig.store.Calls();
        return std::find(calls.begin(), calls.end(), call) - calls.begin();
    };
    EXPECT_EQ(index("readAudio old-session"), 0);
    EXPECT_EQ(rig.store.frames.size(), combined) << "stored and live audio as one stream";
    EXPECT_EQ(rig.diariser.audio_frames, combined) << "the combined recording is finalised";
    EXPECT_LT(index("finalise s1"), index("delete old-session"))
        << "the old session is superseded once the new one finalises";

    // The new session held all of the old one's audio, so a cancel discards
    // the whole consultation
    ASSERT_TRUE(controller.Start(std::nullopt, "older"));
    controller.Cancel();
    const auto calls = rig.store.Calls();
    ASSERT_NE(std::find(calls.begin(), calls.end(), "delete older"), calls.end());
    EXPECT_LT(index("cancel s2"), index("delete older"));
}

TEST(SessionController, StartOptionsReachTheSourceAndTheRecord) {
    Rig rig;
    std::string mic;
    std::optional<ReplaySpec> replay;
    ScriptedSource* source = nullptr;
    auto controller =
        rig.Make([&](const std::optional<ReplaySpec>& spec, const std::string& mic_id) {
            auto made = std::make_unique<ScriptedSource>(Script::kStreamUntilStopped);
            mic = mic_id;
            replay = spec;
            source = made.get();
            return made;
        });

    ASSERT_TRUE(controller.Start(std::nullopt, {}, true,
                                 {"{0.0.1}.{aa}", "Microphone Array (Cirrus Logic)"}));
    EXPECT_EQ(mic, "{0.0.1}.{aa}") << "the chosen microphone is pinned";
    EXPECT_FALSE(replay.has_value());
    EXPECT_EQ(rig.store.last_device_id, "{0.0.1}.{aa}");
    EXPECT_EQ(rig.store.last_device_name, "Microphone Array (Cirrus Logic)");
    controller.SetPaused(true);
    ASSERT_NE(source, nullptr);
    EXPECT_TRUE(source->paused.load());
    controller.Stop();
    EXPECT_FALSE(controller.Running()) << "a stop while paused still ends the session";
    EXPECT_EQ(rig.store.Calls().back(), "finalise s1");

    ASSERT_TRUE(controller.Start(ReplaySpec{"C:/tracks/elbow.wav", 4.0, true}, {}, true,
                                 {"{0.0.1}.{aa}", "Microphone Array"}));
    controller.Stop();
    ASSERT_TRUE(replay.has_value());
    EXPECT_EQ(replay->path, "C:/tracks/elbow.wav");
    EXPECT_EQ(replay->speed, 4.0);
    EXPECT_TRUE(replay->monitor);
    EXPECT_EQ(rig.store.last_device_id, "") << "a replay carries no device snapshot";
    EXPECT_EQ(rig.store.last_device_name, "");
}

TEST(SessionController, RetainReachesTheStoreAndLeavingSweeps) {
    Rig rig;
    auto controller = rig.Make(Script::kStreamUntilStopped);

    ASSERT_TRUE(controller.Start(std::nullopt, {}, false));
    EXPECT_FALSE(rig.store.last_retain);
    EXPECT_EQ(rig.store.sweeps, 1) << "the previous consultation is left at start";
    controller.Stop();

    controller.Close();
    EXPECT_EQ(rig.store.sweeps, 2) << "and at close";

    ASSERT_TRUE(controller.Start());
    EXPECT_TRUE(rig.store.last_retain) << "the default keeps";
    EXPECT_EQ(rig.store.sweeps, 3);
    controller.Stop();
}

// Diarisation hand-off

TEST(SessionController,
     StopHandsTheCapturedAudioToDiarisationAndStoresOnlyTheAttributedTranscript) {
    Rig rig;
    rig.diariser.clusters = 2;
    rig.diariser.similarities = {0.2, 0.8};
    rig.diariser.speculate_first_turn = true;
    auto controller = rig.Make(Script::kStreamUntilStopped);

    ASSERT_TRUE(controller.Start());
    ASSERT_TRUE(rig.WaitForFrames(kTwoTurnFrames));
    controller.Stop();

    const auto captured = rig.store.frames.size();
    EXPECT_EQ(rig.diariser.settles, 1) << "finalise decodes what capture had not reached";
    EXPECT_EQ(rig.diariser.settled_frames, captured);
    EXPECT_EQ(rig.diariser.calls, 1);
    EXPECT_EQ(rig.diariser.audio_frames, captured) << "the diariser hears exactly the capture";
    EXPECT_EQ(rig.events.progress, (std::vector<std::string>{"transcript", "speakers", "turns"}));
    EXPECT_EQ(rig.store.Calls(),
              (std::vector<std::string>{"begin s1", "replace s1", "finalise s1"}))
        << "the attributed transcript is the only one the store sees, before the seal";

    ASSERT_EQ(rig.store.turns.size(), 2u) << "one merged turn per cluster";
    const auto half = rig.diariser.audio_frames / 2;
    EXPECT_EQ(rig.diariser.takes, 1);
    EXPECT_EQ(rig.store.turns[0].text, "Speculated words.") << "the cache hit stands, tidied";
    EXPECT_EQ(rig.store.turns[1].first_frame, half);
    EXPECT_EQ(rig.store.turns[1].text,
              "Scripted turn 0, " + std::to_string(captured - half) + " frames.")
        << "the miss decodes fresh from its own audio";
    EXPECT_EQ(rig.store.turns[0].speaker, "patient");
    EXPECT_EQ(rig.store.turns[1].speaker, "doctor") << "the nearer cluster to the anchor";

    EXPECT_EQ(rig.diariser.similarity_calls, 1) << "the anchor is consulted once";
    EXPECT_EQ(rig.diariser.accruals.load(), 1) << "without a note lane the named session teaches";
    EXPECT_EQ(rig.diariser.accrued_cluster, 1);
}

TEST(SessionController, ThePrintLearnsNothingFromUnnamedSpeakersOrWhenFrozen) {
    struct Row {
        const char* name;
        int clusters;
        std::vector<double> similarities;
        bool freeze;
        std::vector<std::string> speakers;
    };
    const std::vector<Row> rows = {
        {"one cluster cannot be named", 1, {}, false, {"speaker 1"}},
        // The scripted text carries no lexical role signal either
        {"two clusters and no anchor", 2, {}, false, {"speaker 1", "speaker 2"}},
        {"a frozen print still names the roles", 2, {0.2, 0.8}, true, {"doctor", "patient"}},
    };
    for (const auto& row : rows) {
        SCOPED_TRACE(row.name);
        Rig rig;
        rig.diariser.clusters = row.clusters;
        rig.diariser.similarities = row.similarities;
        auto controller = rig.Make(Script::kStreamUntilStopped);
        if (row.freeze) controller.FreezeAnchor();

        ASSERT_TRUE(controller.Start());
        ASSERT_TRUE(rig.WaitForFrames(kTwoTurnFrames));
        controller.Stop();

        std::vector<std::string> speakers;
        for (const auto& turn : rig.store.turns) speakers.push_back(turn.speaker);
        std::sort(speakers.begin(), speakers.end());  // numbering follows talk time
        EXPECT_EQ(speakers, row.speakers);
        EXPECT_EQ(rig.diariser.accruals.load(), 0);
    }
}

TEST(SessionController, CaptureTicksAdvanceDiarisationApplyCutsAndPrefillTheNote) {
    Rig rig;
    rig.transcriber.clip_cuts = {800, 2400};
    rig.diariser.speculate_transcript = true;
    // One 100 ms window per tick, so the first tick comes almost at once
    auto controller = rig.Make(Script::kStreamUntilStopped,
                               {.advance_frames = LevelMeter::kWindowFrames, .writer = true});

    ASSERT_TRUE(controller.Start());
    ASSERT_TRUE(WaitFor([&] { return rig.diariser.advances.load() >= 2; }))
        << "the tick that produced cuts advances again at once, so a stop never waits on them";
    EXPECT_GE(rig.writer.prepares.load(), 1) << "the weights warm while the session records";
    controller.Stop();

    EXPECT_GT(rig.diariser.advanced_frames, 0u);
    EXPECT_LE(rig.diariser.advanced_frames, rig.store.frames.size())
        << "Advance only ever sees captured audio";
    EXPECT_EQ(rig.diariser.cut_points, (std::vector<std::uint64_t>{800, 2400}))
        << "the cuts reach the diariser once";
    EXPECT_GT(rig.writer.prefills.load(), 0) << "each tick hands the guess to the note lane";
    EXPECT_EQ(rig.writer.last_prefill_speaker, "doctor");
    EXPECT_EQ(rig.diariser.calls, 1) << "the full diarisation runs once, at finalise";
}

TEST(SessionController, FinaliseStagesAndDiariseTimingReachTheMetrics) {
    Rig rig;
    rig.diariser.timing.embed_s = 0.25;
    rig.diariser.timing.embed_misses = 3;
    auto controller = rig.Make(Script::kStreamUntilStopped, {.metrics = true});

    ASSERT_TRUE(controller.Start(ReplaySpec{"x.wav", 4.0, false}));
    ASSERT_TRUE(rig.WaitForFrames(kTwoTurnFrames));
    controller.Stop();

    // The perf report reads these names
    const auto s = rig.registry.Take();
    EXPECT_TRUE(s.replay);
    EXPECT_EQ(s.replay_speed, 4.0);
    for (const char* stage : {"capture joined", "capture settled", "diarised", "voiceprints joined",
                              "diarise voiceprints"}) {
        EXPECT_TRUE(s.stage_seconds.contains(stage)) << stage;
    }
    EXPECT_EQ(s.stage_seconds.at("diarise embed"), 0.25);
    EXPECT_EQ(s.stage_seconds.at("diarise embed misses"), 3);
}

// Note, sheet and title

TEST(SessionController, StopWritesTheNoteThenTheSheetThenTheTitle) {
    Rig rig;
    rig.diariser.clusters = 2;
    rig.diariser.similarities = {0.2, 0.8};
    rig.writer.patient = true;
    rig.writer.block = true;  // holds the note so the print can be seen waiting for it
    {
        auto controller = rig.Make(Script::kStreamUntilStopped, {.writer = true});
        controller.SetNoteOptions({"soap", "concise"});
        ASSERT_TRUE(controller.Start());
        ASSERT_TRUE(rig.WaitForFrames(kTwoTurnFrames));
        controller.Stop();

        ASSERT_TRUE(rig.events.WaitUntil([&] { return !rig.events.note_partials.empty(); }));
        EXPECT_EQ(rig.diariser.accruals.load(), 0) << "the print waits for an accepted note";
        EXPECT_FALSE(controller.Running());
        EXPECT_TRUE(controller.Busy()) << "capture is over but the note is still being written";
        rig.writer.block = false;
        ASSERT_TRUE(rig.events.WaitForNote());
        EXPECT_EQ(controller.LastFinalised(), "s1");
        EXPECT_TRUE(WaitFor([&] { return !controller.Busy(); }))
            << "free once note and sheet are done";
    }  // the controller joins the lane, so the sheet and the title are done

    EXPECT_EQ(rig.events.note_partials,
              (std::vector<std::string>{"the clinica", "the clinical note"}));
    EXPECT_EQ(rig.events.note_ready, "the clinical note");
    EXPECT_TRUE(rig.events.note_failed.empty());
    EXPECT_EQ(rig.events.note_saved_session, "s1");
    EXPECT_EQ(rig.events.note_saved_text, "the clinical note");
    EXPECT_EQ(rig.events.note_saved_revision, 1) << "the note as stored, not as generated";
    ASSERT_EQ(rig.writer.calls.size(), 1u);
    EXPECT_FALSE(rig.writer.calls[0].empty()) << "the writer gets the transcript";
    EXPECT_EQ(rig.writer.last_options.style, "soap");
    EXPECT_EQ(rig.writer.last_options.detail, "concise");
    EXPECT_EQ(rig.store.note, "the clinical note");
    EXPECT_EQ(rig.store.note_style, "soap");
    EXPECT_EQ(rig.store.note_detail, "concise");

    EXPECT_EQ(rig.writer.patient_input, "the clinical note")
        << "the sheet is written from the note";
    EXPECT_EQ(rig.events.patient_partials, (std::vector<std::string>{"Your appointment"}));
    EXPECT_EQ(rig.events.patient_ready, "the patient sheet");
    EXPECT_EQ(rig.store.patient, "the patient sheet");
    EXPECT_EQ(rig.store.Calls(), (std::vector<std::string>{"begin s1", "replace s1", "finalise s1",
                                                           "note s1", "patient s1"}));

    EXPECT_EQ(rig.writer.label_calls.load(), 1);
    EXPECT_EQ(rig.store.label, "Elbow swelling") << "the model's title, sanitised";
    EXPECT_FALSE(rig.store.label_typed);

    EXPECT_EQ(rig.diariser.accruals.load(), 1) << "learned once the note was accepted";
    EXPECT_EQ(rig.diariser.accrued_cluster, 1);
    EXPECT_TRUE(rig.events.storage_faults.empty());
}

TEST(SessionController, NoteLaneFailuresNeverCostWhatCameBefore) {
    struct Row {
        const char* name;
        std::function<void(Rig&)> arrange;
        std::function<void(Rig&)> check;
    };
    const std::vector<Row> rows = {
        {"the writer fails",
         [](Rig& rig) {
             rig.writer.fail = true;
             rig.writer.patient = true;
         },
         [](Rig& rig) {
             EXPECT_EQ(rig.events.note_failed, "generation failed");
             EXPECT_TRUE(rig.events.note_ready.empty());
             EXPECT_TRUE(rig.store.note.empty());
             EXPECT_TRUE(rig.writer.patient_input.empty()) << "no sheet without a note";
             EXPECT_EQ(rig.writer.label_calls.load(), 0);
         }},
        {"the sheet fails",
         [](Rig& rig) {
             rig.writer.patient = true;
             rig.writer.fail_patient = true;
         },
         [](Rig& rig) {
             EXPECT_EQ(rig.events.patient_failed, "patient generation failed");
             EXPECT_TRUE(rig.events.patient_ready.empty());
             EXPECT_EQ(rig.events.note_ready, "the clinical note");
             EXPECT_EQ(rig.store.note, "the clinical note");
             EXPECT_TRUE(rig.store.patient.empty());
         }},
        {"the store refuses documents", [](Rig& rig) { rig.store.refuse_documents = true; },
         [](Rig& rig) {
             EXPECT_EQ(rig.events.note_ready, "the clinical note");
             EXPECT_TRUE(rig.events.note_saved_session.empty())
                 << "nothing stored, nothing to search";
             EXPECT_TRUE(rig.store.note.empty());
             ASSERT_FALSE(rig.events.storage_faults.empty()) << "a full disk reaches the shell";
             EXPECT_EQ(rig.events.storage_faults.front(), "disk full");
         }},
        {"the work after the note throws", [](Rig& rig) { rig.events.note_saved_throws = true; },
         [](Rig& rig) {
             EXPECT_EQ(rig.events.note_ready, "the clinical note");
             EXPECT_TRUE(rig.events.note_failed.empty());
             EXPECT_EQ(rig.store.note, "the clinical note");
         }},
        {"the title is rejected",
         [](Rig& rig) { rig.writer.label_result = "  \"...\"  "; },  // nothing survives
         [](Rig& rig) {
             EXPECT_EQ(rig.writer.label_calls.load(), 1);
             EXPECT_EQ(rig.store.label, "")
                 << "no title beats a bad title; the list shows the date";
         }},
        {"a note-only writer", [](Rig&) {},
         [](Rig& rig) {
             EXPECT_EQ(rig.events.note_ready, "the clinical note");
             EXPECT_TRUE(rig.events.patient_partials.empty()) << "the sheet lane never runs";
             EXPECT_TRUE(rig.writer.patient_input.empty());
         }},
    };
    for (const auto& row : rows) {
        SCOPED_TRACE(row.name);
        Rig rig;
        row.arrange(rig);
        {
            auto controller = rig.Make(Script::kCompleteAfterAudio, {.writer = true});
            ASSERT_TRUE(controller.Start());
            controller.Stop();
            ASSERT_TRUE(rig.events.WaitForNote());
        }  // joins the lane
        row.check(rig);
    }
}

// The title is written after the documents. A consultation opened meanwhile
// is not refused for it
TEST(SessionController, OpenIsNotRefusedWhileTheTitleIsWritten) {
    Rig rig;
    rig.store.turns = {
        {0, 16000 * 30, "doctor", "a stored consultation with enough words to note"}};
    rig.writer.block_label = true;
    auto controller = rig.Make(Script::kStreamUntilStopped, {.writer = true});

    ASSERT_TRUE(controller.Start());
    controller.Stop();
    ASSERT_TRUE(rig.events.WaitForNote());
    ASSERT_TRUE(WaitFor([&] { return rig.writer.label_calls.load() == 1; }))
        << "the title is being written";

    EXPECT_TRUE(controller.Open("past"));

    rig.writer.block_label = false;
    ASSERT_TRUE(WaitFor([&] {
        const std::lock_guard<std::mutex> lock(rig.store.mutex);
        return !rig.store.label.empty();
    }));
    EXPECT_EQ(rig.store.label, "Elbow swelling");
}

TEST(SessionController, AReviewedSessionRegeneratesItsNoteSheetAndSummary) {
    Rig rig;
    rig.store.turns = {
        {0, 16000 * 30, "doctor", "a stored consultation with enough words to note"}};
    rig.store.label = "Elbow swelling";
    rig.store.label_typed = true;
    rig.writer.patient = true;
    const std::string edited = "the note as the clinician edited it";
    {
        auto controller = rig.Make(Script::kStreamUntilStopped, {.writer = true});
        // A call is refused while the lane finishes the previous write
        const auto accepted = [](auto call) { return WaitFor(call); };

        // Any stored session can be summarised, no review needed
        EXPECT_FALSE(controller.WriteSummary("past")) << "no note, nothing to summarise";
        rig.store.note = edited;
        ASSERT_TRUE(controller.WriteSummary("past"));
        ASSERT_TRUE(rig.events.WaitForSummary());
        EXPECT_EQ(rig.events.summary_session, "past");
        EXPECT_EQ(rig.events.summary_text,
                  "A patient in their forties presented with a swollen elbow.");
        EXPECT_EQ(rig.writer.summary_input, edited);
        EXPECT_EQ(rig.store.summary, rig.events.summary_text);
        {
            const std::lock_guard<std::mutex> lock(rig.events.mutex);
            rig.events.summary_done = false;
        }
        rig.writer.fail_summary = true;
        ASSERT_TRUE(accepted([&] { return controller.WriteSummary("past"); }));
        ASSERT_TRUE(rig.events.WaitForSummary());
        EXPECT_EQ(rig.events.summary_failed, "summary generation failed");

        EXPECT_FALSE(controller.RegeneratePatient()) << "nothing open yet";
        ASSERT_TRUE(accepted([&] { return controller.Open("past"); }));
        EXPECT_EQ(controller.LastFinalised(), "past");

        ASSERT_TRUE(accepted([&] { return controller.RegeneratePatient(); }));
        ASSERT_TRUE(rig.events.WaitForPatient());
        EXPECT_EQ(rig.writer.patient_input, edited)
            << "the sheet regenerates from the stored note, edits included";
        EXPECT_EQ(rig.store.patient, "the patient sheet");

        ASSERT_TRUE(accepted([&] { return controller.RegenerateNote({"soap", "concise"}); }));
        ASSERT_TRUE(rig.events.WaitForNote());
        EXPECT_EQ(rig.store.note, "the clinical note");
        EXPECT_EQ(rig.store.note_style, "soap");
        const auto calls = rig.store.Calls();
        EXPECT_NE(std::find(calls.begin(), calls.end(), "note past"), calls.end())
            << "the note is stored against the opened session";

        controller.Close();
        EXPECT_TRUE(controller.LastFinalised().empty()) << "leaving ends the review";
        EXPECT_FALSE(controller.RegenerateNote({"prose", "standard"})) << "closed";
        EXPECT_FALSE(controller.RegeneratePatient()) << "closed";
    }  // joins the lane, so the title step has run

    EXPECT_EQ(rig.store.label, "Elbow swelling");
    EXPECT_TRUE(rig.store.label_typed);
    EXPECT_EQ(rig.writer.label_calls.load(), 0) << "a typed label is never regenerated";
}

TEST(SessionController, RegenerateAndOpenAreRefusedWhenTheyCannotRun) {
    {
        Rig rig;
        rig.store.turns = {{0, 16000, "doctor", "words"}};
        auto controller = rig.Make(Script::kStreamUntilStopped);
        ASSERT_TRUE(controller.Open("past"));
        EXPECT_FALSE(controller.RegenerateNote({"prose", "standard"}))
            << "no writer: refused, not crashed";
    }

    Rig rig;
    auto controller = rig.Make(Script::kStreamUntilStopped, {.writer = true});
    EXPECT_FALSE(controller.RegenerateNote({})) << "nothing finalised yet";

    rig.store.refuse_read_turns = true;
    EXPECT_FALSE(controller.Open("nope")) << "an unknown session";
    rig.store.refuse_read_turns = false;

    ASSERT_TRUE(controller.Open("past"));
    ASSERT_TRUE(controller.Start());
    EXPECT_FALSE(controller.Open("past")) << "not while recording";
    EXPECT_FALSE(controller.RegenerateNote({})) << "not while recording";
    controller.Stop();
    ASSERT_TRUE(rig.events.WaitForNote());
    EXPECT_EQ(controller.LastFinalised(), "s1") << "a finalise sets its own target";

    rig.events.ResetNote();
    rig.store.turns = {{0, 16000, "doctor", "words"}};  // regenerate needs a stored transcript
    ASSERT_TRUE(WaitFor([&] { return controller.RegenerateNote({"soap", "concise"}); }));
    ASSERT_TRUE(rig.events.WaitForNote());
    ASSERT_EQ(rig.writer.calls.size(), 2u);
    EXPECT_EQ(rig.writer.last_options.style, "soap");
    EXPECT_EQ(rig.writer.last_options.detail, "concise");
    EXPECT_EQ(rig.store.note, "the clinical note") << "the regenerated note is re-saved";

    controller.Close();
    EXPECT_EQ(controller.LastFinalised(), "s1")
        << "Record closed the review, so leaving keeps the finalised session";
}

TEST(SessionController, AThinOrMissingTranscriptIsRefusedWithoutAskingTheModel) {
    struct Row {
        const char* name;
        bool diarise_fails;
        std::vector<std::string> calls;
        std::string refusal;
    };
    const std::vector<Row> rows = {
        {"a thin transcript",
         false,
         {"begin s1", "replace s1", "finalise s1"},
         "5 words; a note needs at least 25"},
        // The session is never lost to a diarisation failure
        {"diarisation failed, so no transcript",
         true,
         {"begin s1", "finalise s1"},
         "0 words; a note needs at least 25"},
    };
    for (const auto& row : rows) {
        SCOPED_TRACE(row.name);
        Rig rig;
        rig.diariser.fail_diarise = row.diarise_fails;
        rig.writer.patient = true;
        auto controller = rig.Make(Script::kStreamUntilStopped,
                                   {.writer = true, .min_note_words = NoteLane::kMinNoteWords});

        ASSERT_TRUE(controller.Start());
        ASSERT_TRUE(rig.WaitForFrames(kTwoTurnFrames));
        controller.Stop();
        ASSERT_TRUE(rig.events.WaitForNote());

        EXPECT_EQ(rig.store.Calls(), row.calls);
        EXPECT_EQ(controller.LastFinalised(), "s1");
        EXPECT_GE(rig.diariser.discards, 1);
        EXPECT_TRUE(rig.writer.calls.empty()) << "the model must never see a transcript this thin";
        EXPECT_EQ(rig.events.note_refused, row.refusal);
        EXPECT_FALSE(rig.events.note_refused_overridable)
            << "insisting would make the model fabricate";
        EXPECT_TRUE(rig.events.note_ready.empty());
        EXPECT_TRUE(rig.events.patient_ready.empty()) << "no sheet without a note";
        EXPECT_TRUE(rig.events.note_failed.empty()) << "a thin recording is not an error state";
        EXPECT_TRUE(rig.store.note.empty());
    }
}

TEST(SessionController, ANoteStillWritingIsCancelledByCancelOrDestruction) {
    {
        Rig rig;
        auto controller = rig.Make(Script::kCompleteAfterAudio, {.writer = true});
        ASSERT_TRUE(controller.Start());
        controller.Cancel();
        EXPECT_TRUE(rig.writer.calls.empty()) << "a cancel writes no note";
        EXPECT_TRUE(rig.events.note_partials.empty());
    }

    Rig rig;
    rig.writer.block = true;
    {
        auto controller = rig.Make(Script::kCompleteAfterAudio, {.writer = true});
        ASSERT_TRUE(controller.Start());
        controller.Stop();
        // Destruction must catch the write in flight rather than before it starts
        ASSERT_TRUE(rig.events.WaitUntil([&] { return !rig.events.note_partials.empty(); }));
    }
    EXPECT_TRUE(rig.writer.cancelled.load());
    EXPECT_EQ(rig.events.note_ready, "interrupted") << "an interrupted write returns what it had";
}

// Refusal and override

TEST(SessionController, ARefusedRecordingYieldsNothingAndIsErasedAtClose) {
    Rig rig;
    rig.diariser.clusters = 2;
    rig.diariser.similarities = {0.9, 0.3};
    rig.writer.patient = true;
    rig.writer.result = "NOT A CONSULTATION: a cooking video with one speaker";
    auto controller = rig.Make(Script::kStreamUntilStopped, {.writer = true});

    ASSERT_TRUE(controller.Start());
    ASSERT_TRUE(rig.WaitForFrames(kTwoTurnFrames));
    controller.Stop();
    ASSERT_TRUE(rig.events.WaitForNote());
    {
        const std::lock_guard<std::mutex> lock(rig.events.mutex);
        EXPECT_EQ(rig.events.note_refused, "a cooking video with one speaker");
        EXPECT_TRUE(rig.events.note_refused_overridable) << "the clinician may insist";
        EXPECT_TRUE(rig.events.note_ready.empty()) << "no note reached the shell";
        EXPECT_TRUE(rig.events.note_partials.empty()) << "the refusal never streamed as a note";
    }
    EXPECT_EQ(rig.writer.patient_input, "") << "no sheet";
    EXPECT_EQ(rig.writer.label_calls.load(), 0) << "no title";
    EXPECT_EQ(rig.diariser.accruals.load(), 0) << "the print learned nothing";
    for (const auto& call : rig.store.Calls()) {
        EXPECT_EQ(call.find("note"), std::string::npos) << call;
    }

    controller.Close();
    const auto calls = rig.store.Calls();
    EXPECT_NE(std::find(calls.begin(), calls.end(), "delete s1"), calls.end())
        << "a refused recording is not kept";
    EXPECT_TRUE(controller.LastFinalised().empty());
}

TEST(SessionController, AnInsistedRewriteIsDeliveredAndTheSessionKept) {
    Rig rig;
    rig.diariser.clusters = 2;
    rig.writer.result = "NOT A CONSULTATION: a cooking video";
    auto controller = rig.Make(Script::kStreamUntilStopped, {.writer = true});

    ASSERT_TRUE(controller.Start());
    ASSERT_TRUE(rig.WaitForFrames(kTwoTurnFrames));
    controller.Stop();
    ASSERT_TRUE(rig.events.WaitForNote());
    ASSERT_FALSE(rig.events.note_refused.empty());

    // The same lane, told not to refuse, delivers whatever the model says
    rig.events.ResetNote();
    rig.writer.result = "NOT A CONSULTATION: the model still says so";
    note::NoteOptions confirmed;
    confirmed.confirmed = true;
    ASSERT_TRUE(WaitFor([&] { return controller.RegenerateNote(confirmed); }));
    ASSERT_TRUE(rig.events.WaitForNote());
    {
        const std::lock_guard<std::mutex> lock(rig.events.mutex);
        EXPECT_TRUE(rig.events.note_refused.empty());
        EXPECT_EQ(rig.events.note_ready, "NOT A CONSULTATION: the model still says so");
    }

    controller.Close();
    for (const auto& call : rig.store.Calls()) {
        EXPECT_NE(call, "delete s1") << "the clinician insisted, so the session stays";
    }
}

// A restored consultation may come without its transcript, and a review is a kept record
TEST(SessionController, AReviewedSessionIsNeverRewrittenFromNothingNorErasedByARefusal) {
    Rig rig;
    rig.writer.result = "NOT A CONSULTATION: a cooking video";
    auto controller = rig.Make(Script::kStreamUntilStopped, {.writer = true});

    ASSERT_TRUE(controller.Open("past"));
    EXPECT_FALSE(controller.RegenerateNote({"prose", "standard"})) << "no transcript to write from";
    EXPECT_TRUE(rig.writer.calls.empty());

    rig.store.turns = {
        {0, 16000 * 30, "doctor", "a stored consultation with enough words to note"}};
    ASSERT_TRUE(controller.RegenerateNote({"prose", "standard"}));
    ASSERT_TRUE(rig.events.WaitForNote());
    {
        const std::lock_guard<std::mutex> lock(rig.events.mutex);
        EXPECT_EQ(rig.events.note_refused, "a cooking video");
    }

    controller.Close();
    for (const auto& call : rig.store.Calls()) {
        EXPECT_NE(call, "delete past") << "a refused rewrite never erases the reviewed session";
    }
    EXPECT_TRUE(controller.LastFinalised().empty()) << "leaving still ends the review";
}

// Enrolment

std::optional<std::tuple<bool, std::string, double>> WaitEnrol(Rig& rig) {
    rig.events.WaitUntil([&] { return rig.events.enrol_done.has_value(); });
    const std::lock_guard<std::mutex> lock(rig.events.mutex);
    return rig.events.enrol_done;
}

TEST(SessionController, EnrolmentFinishedEarlyReplacesTheAnchorAndStoresNothing) {
    Rig rig;
    auto controller = rig.Make(Script::kStreamUntilStopped);

    ASSERT_TRUE(controller.StartEnrolment(120.0, {}, 0.1));  // a cap far beyond the test
    ASSERT_TRUE(rig.events.WaitUntil([&] {
        return !rig.events.enrol_progress.empty() &&
               rig.events.enrol_progress.back().speech_s >= 0.2;
    })) << "the level and speech so far are reported";
    controller.FinishEnrolment();
    const auto done = WaitEnrol(rig);

    ASSERT_TRUE(done.has_value());
    EXPECT_TRUE(std::get<0>(*done)) << std::get<1>(*done);
    EXPECT_GE(std::get<2>(*done), 0.1);
    EXPECT_LT(std::get<2>(*done), 60.0) << "stopped at Finish, not at the cap";
    EXPECT_EQ(rig.diariser.replaced, rig.diariser.voiceprint);
    EXPECT_GT(rig.diariser.replaced_at, 1'700'000'000u) << "stamped with the wall clock";
    EXPECT_GE(rig.diariser.embedded_frames, 1600u) << "the gated speech reached the embedder";
    EXPECT_TRUE(rig.events.levels.empty()) << "not as session levels";
    EXPECT_TRUE(rig.store.Calls().empty()) << "an enrolment stores nothing";
}

TEST(SessionController, AnEnrolmentThatDoesNotCompleteLeavesTheAnchorAlone) {
    {
        Rig rig;
        auto controller = rig.Make(Script::kStreamUntilStopped);
        ASSERT_TRUE(controller.Start());
        EXPECT_FALSE(controller.StartEnrolment(0.5)) << "the microphone is the session's";
        controller.Stop();

        ASSERT_TRUE(controller.StartEnrolment(2.0, {}, 0.1));
        EXPECT_FALSE(controller.Start()) << "the microphone is the enrolment's";
        controller.CancelEnrolment();
        const auto done = WaitEnrol(rig);
        ASSERT_TRUE(done.has_value());
        EXPECT_FALSE(std::get<0>(*done));
        EXPECT_EQ(std::get<1>(*done), "cancelled");
        EXPECT_TRUE(rig.diariser.replaced.empty()) << "a cancelled enrolment changes nothing";
        ASSERT_TRUE(controller.Start()) << "and the microphone is free again";
        controller.Stop();
    }

    Rig rig;
    auto controller = rig.Make(Script::kDieAfterAudio);
    ASSERT_TRUE(controller.StartEnrolment(5.0, {}, 0.01));
    const auto done = WaitEnrol(rig);
    ASSERT_TRUE(done.has_value());
    EXPECT_FALSE(std::get<0>(*done));
    EXPECT_EQ(std::get<1>(*done), "microphone unplugged");
    EXPECT_TRUE(rig.diariser.replaced.empty());
}

}  // namespace
}  // namespace clinicavt::session
