#pragma once

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
#include "core/session/session_controller.hpp"

// The session controller's collaborators as fakes, shared by its tests
namespace clinicavt::session::test_harness {

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
inline std::vector<float> Window() {
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
                    sink.OnAudio(window, 0);
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                sink.OnEnd({SourceEndReason::kStopped, ""});
                return;
        }
    }

    void RequestStop() override {
        stop_.store(true);
    }

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

    // Call before a second write so the first's outcome is cleared
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
    std::string last_started_at;
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
        last_started_at = meta.started_at;
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

    // Counted separately so the calls sequence stays exact
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

    // An import's speech pass, reported in two halves
    int speech_passes = 0;

    void FindSpeech(std::span<const float>, const std::function<void(double)>& progress,
                    const diar::StopFn&) override {
        ++speech_passes;
        progress(0.5);
        progress(1.0);
    }

    // With decode_halves the catch-up decodes the audio in two halves, as far as stop allows.
    // mid_settle runs between them
    bool decode_halves = false;
    std::function<void()> mid_settle;

    void Settle(std::span<const float> audio, const diar::DecodeClipFn& decode,
                const diar::StopFn& stop) override {
        ++settles;
        settled_frames = audio.size();
        if (!decode_halves) return;
        const std::size_t half = audio.size() / 2;
        for (const auto& [first, end] :
             {std::pair{std::size_t{0}, half}, std::pair{half, audio.size()}}) {
            if (stop()) return;
            (void)decode(audio.subspan(first, end - first), first);
            if (first == 0 && mid_settle) mid_settle();
        }
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

inline SourceFactory FactoryFor(Script script) {
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

inline std::optional<std::tuple<bool, std::string, double>> WaitEnrol(Rig& rig) {
    rig.events.WaitUntil([&] { return rig.events.enrol_done.has_value(); });
    const std::lock_guard<std::mutex> lock(rig.events.mutex);
    return rig.events.enrol_done;
}

// Import callbacks from its thread
struct ImportLog {
    std::mutex mutex;
    std::vector<std::pair<ImportStage, int>> progress;
    std::optional<std::pair<store::SessionId, std::string>> done;

    ImportReport Report() {
        return {.progress =
                    [this](const store::SessionId&, ImportStage stage, int percent) {
                        const std::lock_guard<std::mutex> lock(mutex);
                        progress.emplace_back(stage, percent);
                    },
                .done =
                    [this](const store::SessionId& id, const std::string& error) {
                        const std::lock_guard<std::mutex> lock(mutex);
                        done.emplace(id, error);
                    }};
    }

    bool Wait() {
        return WaitFor([this] {
            const std::lock_guard<std::mutex> lock(mutex);
            return done.has_value();
        });
    }
};

// Reports reading halfway, then done
inline ImportRead Reads(std::vector<float> recording) {
    return [recording = std::move(recording)](const std::function<void(double)>& progress) {
        progress(0.5);
        progress(1.0);
        return recording;
    };
}

}  // namespace clinicavt::session::test_harness
