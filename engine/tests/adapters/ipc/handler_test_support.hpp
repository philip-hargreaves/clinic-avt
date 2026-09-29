#pragma once

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "adapters/demo/json_sample_source.hpp"
#include "adapters/diarisation/scripted_diariser.hpp"
#include "adapters/interfaces/recording_reader.hpp"
#include "adapters/ipc/handlers.hpp"
#include "adapters/ipc/pipe_client.hpp"
#include "adapters/ipc/pipe_server.hpp"
#include "adapters/storage/reflection_json.hpp"
#include "adapters/storage/sqlite_session_store.hpp"
#include "adapters/transcription/scripted_transcriber.hpp"
#include "adapters/translate/translate_lane.hpp"
#include "adapters/vad/passthrough_vad.hpp"

// Helpers shared by the handler tests
namespace clinicavt::ipc::handler_test {

inline const json& ResultOf(const std::variant<json, Error>& outcome) {
    return std::get<json>(outcome);
}

inline json LoadFixture(const std::string& name) {
    std::ifstream in(std::string(CLINICAVT_FIXTURE_DIR) + "/" + name);
    if (!in.is_open()) throw std::runtime_error("missing fixture: " + name);
    return json::parse(in);
}

struct TempDir {
    std::filesystem::path path;

    explicit TempDir(const std::string& name)
        : path(std::filesystem::temp_directory_path() / ("clinicavt-handlers-" + name)) {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

inline void StageModel(const std::filesystem::path& root, const std::string& id,
                       const std::string& task, const std::string& tier,
                       const std::string& extra = "") {
    std::filesystem::create_directories(root / id);
    std::ofstream(root / id / "manifest.json")
        << R"({"manifestVersion": 1, "id": ")" << id << R"(",)" << extra << R"( "task": ")" << task
        << R"(", "tier": ")" << tier << R"(", "licence": "MIT",)"
        << R"( "runtime": {"device": "GPU"}, "files": {"model.xml": "00"}})";
}

// A store in a folder of its own, with the services the handlers call over it
struct SessionStoreFixture {
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("clinicavt-handlers-sessions-" +
         std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "-" +
         ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::unique_ptr<clinicavt::store::SqliteSessionStore> store =
        std::make_unique<clinicavt::store::SqliteSessionStore>(root, std::chrono::hours(1));
    clinicavt::store::JsonReflectionCodec codec;
    clinicavt::demo::JsonSampleSource samples{CLINICAVT_DEMO_DIR};
    clinicavt::records::SessionRecords records{*store};
    clinicavt::records::Reflections reflections{*store, codec};
    clinicavt::demo::DemoSamples demo{*store, codec, samples};

    ~SessionStoreFixture() {
        store.reset();
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    // Adds a turn so it isn't treated as a cleared consultation
    std::string AddFinalisedSession() const {
        const auto id = store->Begin({16000, "", ""});
        store->ReplaceTurns(id, std::vector<asr::Turn>{{0, 16000, "", "how is the elbow"}});
        store->Finalise(id);
        return id;
    }
};

// Polls for state set on the import thread
template <typename Pred>
bool WaitUntil(Pred done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

// fail reaches both calls, fail_decode only the decode on the import's thread
struct FakeReader : clinicavt::audio::IRecordingReader {
    clinicavt::audio::RecordingInfo info;
    std::vector<float> audio;
    std::function<void()> fail;
    std::function<void()> fail_decode;
    std::filesystem::path last_path;
    std::atomic<int> decodes{0};

    clinicavt::audio::RecordingInfo Inspect(const std::filesystem::path& path) override {
        last_path = path;
        if (fail) fail();
        return info;
    }

    std::vector<float> Decode(const std::filesystem::path&,
                              const clinicavt::audio::ReadProgress& progress) override {
        ++decodes;
        if (fail) fail();
        if (fail_decode) fail_decode();
        progress(1.0);
        return audio;
    }
};

// Audio source giving silence until stopped
struct SilentSource : clinicavt::audio::IAudioSource {
    std::atomic<bool> stop{false};

    void Run(clinicavt::audio::IAudioSink& sink) override {
        const std::vector<float> window(1600, 0.0F);
        while (!stop.load()) {
            sink.OnAudio(window, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        sink.OnEnd({clinicavt::audio::SourceEndReason::kStopped, ""});
    }

    void RequestStop() override {
        stop.store(true);
    }
};

struct QuietEvents : clinicavt::session::ISessionEvents {
    void OnLevel(const clinicavt::audio::LevelReading&) override {}
    void OnInterrupted(clinicavt::audio::SourceEndReason, const std::string&) override {}
};

// Everything the import's thread uses is declared before the controller, which joins it
struct ImportRig {
    SessionStoreFixture fixture;
    QuietEvents events;
    clinicavt::asr::ScriptedTranscriber transcriber;
    clinicavt::audio::PassthroughVad vad;
    clinicavt::diar::ScriptedDiariser diariser;
    FakeReader reader;
    std::mutex mutex;
    std::vector<std::pair<std::string, json>> pushed;
    clinicavt::session::SessionController controller{
        [](const auto&, const auto&) { return std::make_unique<SilentSource>(); },
        events,
        *fixture.store,
        transcriber,
        vad,
        diariser};

    std::variant<json, Error> Import(const json& params,
                                     const std::vector<std::string>& missing = {}) {
        return HandleSessionImport(
            reader, controller, nullptr,
            [this](const std::string& method, json body) {
                const std::lock_guard<std::mutex> lock(mutex);
                pushed.emplace_back(method, std::move(body));
            },
            params, missing);
    }

    // Every push but progress
    std::vector<std::string> Methods() {
        const std::lock_guard<std::mutex> lock(mutex);
        std::vector<std::string> methods;
        for (const auto& [method, body] : pushed) {
            if (method != "session/importProgress") methods.push_back(method);
        }
        return methods;
    }

    // The progress pushed, as "stage percent"
    std::vector<std::string> Progress() {
        const std::lock_guard<std::mutex> lock(mutex);
        std::vector<std::string> progress;
        for (const auto& [method, body] : pushed) {
            if (method == "session/importProgress") {
                progress.push_back(body["stage"].get<std::string>() + " " +
                                   std::to_string(body["percent"].get<int>()));
            }
        }
        return progress;
    }

    // Waits for session/imported or session/importFailed
    std::optional<std::pair<std::string, json>> WaitForEnd() {
        std::optional<std::pair<std::string, json>> end;
        (void)WaitUntil([&] {
            const std::lock_guard<std::mutex> lock(mutex);
            for (const auto& entry : pushed) {
                if (entry.first == "session/imported" || entry.first == "session/importFailed") {
                    end = entry;
                }
            }
            return end.has_value();
        });
        return end;
    }
};

// Client end of a pipe standing in for the shell, for methods registered on `server` and the
// notifications pushed through it. The pipe name includes the test name because ctest may run
// tests in parallel
class WireShell {
    std::wstring name_ =
        L"\\\\.\\pipe\\LOCAL\\clinicavt-wire-" +
        std::filesystem::path(::testing::UnitTest::GetInstance()->current_test_info()->name())
            .wstring() +
        L"-" + std::to_wstring(GetCurrentProcessId());

   public:
    PipeServer server{name_};

    ~WireShell() {
        client_.Close();
        if (engine_.joinable()) engine_.join();
    }

    // Once the methods are registered
    void Connect() {
        engine_ = std::thread([this] {
            if (server.AwaitClient(std::chrono::seconds(5)) == PipeServer::Accept::kClient) {
                server.Serve();
            }
        });
        for (int attempt = 0; attempt < 200 && !client_.Open(name_); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        ASSERT_TRUE(client_.IsOpen());
    }

    // Sends the request as written and returns the reply with its id, or null after 10 s.
    // Notifications that arrive meanwhile are kept for Notification
    json Call(const json& request) {
        if (!client_.Write(EncodeFrame(request.dump()))) return nullptr;
        json reply;
        (void)Poll([&] {
            for (auto it = frames_.begin(); it != frames_.end(); ++it) {
                if (it->contains("id") && (*it)["id"] == request["id"]) {
                    reply = std::move(*it);
                    frames_.erase(it);
                    return true;
                }
            }
            return false;
        });
        return reply;
    }

    // The first notification of `method` not yet taken, waiting up to 10 s
    std::optional<json> Notification(const std::string& method) {
        std::optional<json> found;
        (void)Poll([&] {
            for (auto it = frames_.begin(); it != frames_.end(); ++it) {
                if (!it->contains("id") && (*it)["method"] == method) {
                    found = std::move(*it);
                    frames_.erase(it);
                    return true;
                }
            }
            return false;
        });
        return found;
    }

   private:
    template <typename Found>
    bool Poll(Found found) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;) {
            client_.Read();
            while (const auto frame = client_.NextFrame()) frames_.push_back(json::parse(*frame));
            if (found()) return true;
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    PipeClient client_;
    std::thread engine_;
    std::vector<json> frames_;
};

// The request with its session id replaced, since the store picks ids
inline json ForSession(json request, const std::string& id) {
    request["params"]["id"] = id;
    return request;
}

// Object keys, for replies whose values vary from run to run
inline std::vector<std::string> KeysOf(const json& object) {
    std::vector<std::string> keys;
    for (const auto& [key, value] : object.items()) keys.push_back(key);
    return keys;
}

// Translates everything to the same Polish sentence, in two streamed parts
struct FakeTranslator : clinicavt::translate::ITranslator {
    bool fail = false;

    std::vector<std::string> Languages() override {
        return {"French", "Polish", "Romanian"};
    }

    std::string Translate(const std::string&, const std::string&,
                          const Progress& progress) override {
        if (fail) throw std::runtime_error("translation failed");
        progress("Odpoczywaj");
        // Parts sent apart in time so the stream has a measurable rate
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        progress("Odpoczywaj łokieć");
        return "Odpoczywaj łokieć i przyjmuj ibuprofen z jedzeniem.";
    }
};

// Answers at once, so a regeneration finishes within the test
struct InstantWriter : clinicavt::note::INoteWriter {
    std::string Write(const std::vector<clinicavt::asr::Turn>&, const clinicavt::note::NoteOptions&,
                      const Progress&) override {
        return "Swollen left elbow for a week. No injury.";
    }

    bool WritesPatient() const override {
        return true;
    }

    std::string WritePatient(const std::string&, const Progress&) override {
        return "Rest the elbow and take ibuprofen with food.";
    }

    std::string WriteSummary(const std::string&) override {
        return "A patient in their forties with a swollen elbow.";
    }
};

// Every service RegisterMethods takes, over fakes and a note writer. The controller is declared
// last, so it is destroyed before anything it uses
struct ServicesRig {
    SessionStoreFixture fixture;
    TempDir model_root{std::string("models-") +
                       ::testing::UnitTest::GetInstance()->current_test_info()->name()};
    TempDir anchor_root{std::string("anchors-") +
                        ::testing::UnitTest::GetInstance()->current_test_info()->name()};
    QuietEvents events;
    clinicavt::asr::ScriptedTranscriber transcriber;
    clinicavt::audio::PassthroughVad vad;
    clinicavt::diar::ScriptedDiariser diariser;
    InstantWriter writer;
    FakeTranslator translator;
    FakeReader reader;
    clinicavt::metrics::Registry registry;
    clinicavt::models::ModelStore models{model_root.path};
    clinicavt::diar::AnchorStore anchors{anchor_root.path};
    clinicavt::translate::TranslateLane translate_lane{translator,
                                                       [](const std::string&, const json&) {}};
    clinicavt::session::SessionController controller{
        [](const auto&, const auto&) { return std::make_unique<SilentSource>(); },
        events,
        *fixture.store,
        transcriber,
        vad,
        diariser,
        std::chrono::seconds(3),
        std::uint64_t{5} * clinicavt::audio::kSampleRate,
        &writer,
        &registry,
        0};

    EngineServices Services(AsrSwitch switch_asr = {}) {
        return {.controller = controller,
                .models = models,
                .sessions = *fixture.store,
                .records = fixture.records,
                .reflections = fixture.reflections,
                .demo = fixture.demo,
                .metrics = &registry,
                .translator = &translator,
                .translate_lane = &translate_lane,
                .anchors = &anchors,
                .switch_asr = std::move(switch_asr),
                .recordings = &reader,
                .allow_replay = true};
    }

    // Waits for the note lane, which refuses new work while it writes
    bool WaitIdle() {
        return WaitUntil([&] { return !controller.Busy(); });
    }
};

}  // namespace clinicavt::ipc::handler_test
