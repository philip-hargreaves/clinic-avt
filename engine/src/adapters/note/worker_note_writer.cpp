#include "adapters/note/worker_note_writer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <nlohmann/json.hpp>

#include "adapters/ipc/framing.hpp"
#include "adapters/ipc/messages.hpp"
#include "adapters/ipc/pipe_client.hpp"
#include "adapters/models/model_store.hpp"
#include "adapters/system/child_process.hpp"
#include "adapters/system/gpu_lease.hpp"
#include "core/common/log.hpp"
#include "core/note/model_failure.hpp"

namespace clinicavt::note {

using nlohmann::json;

struct WorkerNoteWriter::Impl {
    std::filesystem::path host_exe;
    std::filesystem::path models_root;
    std::filesystem::path prompt_path;
    const models::ModelStore* store;

    std::mutex state_mutex;     // guards spawn and the handles
    std::mutex write_mutex;     // frames are written whole
    std::mutex read_mutex;      // one pipe reader at a time, the attempt or the watcher
    system::ChildProcess host;  // in a kill-on-close job, so it dies with the engine
    std::vector<system::ChildProcess> stuck;  // hosts that would not exit, held so none is killed
    ipc::PipeClient pipe;
    std::int64_t next_id = 1;
    // Process-wide counter: an exiting host keeps its pipe name briefly, so names
    // are never reused
    static inline std::atomic<int> spawn_count{0};
    bool closing = false;
    bool respawning = false;                  // under state_mutex: Run is between attempts
    std::atomic<bool> attempt_active{false};  // set while the note thread reads the pipe
    // Id of the unanswered prefill, 0 if none. Limited to one so a host that stops
    // reading cannot fill the pipe and block capture
    std::atomic<std::int64_t> prefill_pending{0};

    // Guards tier and residency. Never held across calls into the host or listener
    mutable std::mutex lane_mutex;
    NoteModelState state;
    Listener listener;
    // Under lane_mutex: last load failure reason and tiers whose cache was already rebuilt once
    LoadFailure last_failure = LoadFailure::kOther;
    bool crashed_loading = false;
    std::vector<std::string> cache_rebuilt;
    std::thread watcher;  // reads load outcomes while no attempt is reading
    std::atomic<bool> watch_stop{false};

    // Generation streams partials constantly, so this much silence means the worker
    // is stuck in a driver call and needs a respawn. A request queued behind a load
    // (hash + compile of a 19 GB model: minutes) has its own longer bound
    static constexpr DWORD kInactivityTimeoutMs = 120'000;
    static constexpr DWORD kLoadTimeoutMs = 20 * 60'000;
    // A host that loses its pipe cancels and exits; slowest measured exit 3.4 s
    // after an unfinished prefill
    static constexpr DWORD kExitGraceMs = 15'000;

    bool WorkerAlive() const {
        return host.Alive() && pipe.IsOpen();
    }

    // False if the host would not exit (stuck in a driver call). It is then held in stuck
    bool CloseWorker() {
        pipe.Close();
        prefill_pending = 0;
        if (host.End(kExitGraceMs)) return true;
        log::Printf("clinicavt-engine: note host %lu did not exit; leaving it\n", host.Pid());
        stuck.push_back(std::move(host));
        host = {};
        system::GpuLease::Global().MarkWedged();
        return false;
    }

    // Loads cannot be cancelled, so wait for the load to settle before closing.
    // Bounded by the load timeout
    void AwaitLoad() {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(kLoadTimeoutMs);
        while (Phase() == NoteModelState::Phase::kLoading && host.Alive() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    std::string Tier() const {
        std::lock_guard<std::mutex> lock(lane_mutex);
        return state.tier;
    }

    // Spawns the host and connects its pipe. Throws if the host cannot start,
    // which shows as a failed note
    void EnsureWorker() {
        std::lock_guard<std::mutex> lock(state_mutex);
        if (WorkerAlive()) {
            return;
        }
        CloseWorker();

        const std::wstring pipe_path = L"\\\\.\\pipe\\LOCAL\\clinicavt-note-" +
                                       std::to_wstring(GetCurrentProcessId()) + L"-" +
                                       std::to_wstring(++spawn_count);
        const std::string tier = Tier();
        const std::wstring args = L"\"" + pipe_path + L"\" \"" + models_root.wstring() + L"\" \"" +
                                  prompt_path.wstring() + L"\" \"" +
                                  std::wstring(tier.begin(), tier.end()) + L"\"";
        try {
            host = system::ChildProcess::Spawn(host_exe, args, {.exempt_from_throttling = true});
        } catch (const std::exception&) {
            throw std::runtime_error("note worker failed to start");
        }
        // The host creates the pipe before loading models, so connecting is fast
        for (int attempt = 0; attempt < 150; ++attempt) {
            if (pipe.Open(pipe_path)) break;
            if (host.WaitFor(100)) break;  // died before serving
        }
        if (!pipe.IsOpen()) {
            CloseWorker();
            throw std::runtime_error("note worker pipe did not open");
        }
        if (pipe.ServerPid() != host.Pid()) {
            CloseWorker();
            throw std::runtime_error("note worker pipe is not the spawned process");
        }
    }

    // Set `pending` before writing so the reply cannot arrive first
    std::int64_t Send(const std::string& method, json params,
                      std::atomic<std::int64_t>* pending = nullptr) {
        const std::int64_t id = next_id++;
        if (pending != nullptr) *pending = id;
        const std::string frame =
            ipc::EncodeFrame(ipc::Serialize(ipc::MakeRequest(id, method, std::move(params))));
        std::lock_guard<std::mutex> lock(write_mutex);
        if (!pipe.Write(frame)) {
            throw std::runtime_error("note worker went away");
        }
        return id;
    }

    // Replies have no method. A prefill reply clears prefill_pending
    void OnReply(const json& message) {
        if (message.contains("id") && message["id"].is_number_integer()) {
            std::int64_t expected = message["id"].get<std::int64_t>();
            prefill_pending.compare_exchange_strong(expected, 0);
        }
    }

    void Transition(const std::function<void(NoteModelState&)>& mutate) {
        NoteModelState snapshot;
        Listener notify;
        {
            std::lock_guard<std::mutex> lock(lane_mutex);
            mutate(state);
            snapshot = state;
            notify = listener;
        }
        if (notify) notify(snapshot);
    }

    NoteModelState::Phase Phase() const {
        std::lock_guard<std::mutex> lock(lane_mutex);
        return state.phase;
    }

    NoteModelState State() const {
        std::lock_guard<std::mutex> lock(lane_mutex);
        return state;
    }

    // Fills in the tier's model and whether its compile cache exists. Throws the
    // store error if no model claims the tier
    void Describe(const std::string& tier, NoteModelState& into) const {
        if (store == nullptr) {
            into.id.clear();
            into.name.clear();
            into.first_use = false;
            return;
        }
        const models::ModelInfo& info = store->Resolve("note", tier);
        into.id = info.id;
        into.name = info.name;
        into.first_use = !models::Compiled(info);
    }

    // Handles the host's two load events, from either reader
    void OnHostEvent(const std::string& event, const json& params) {
        if (event == "loaded") {
            Transition([&params](NoteModelState& s) {
                s.phase = NoteModelState::Phase::kReady;
                s.detail.clear();
                s.seconds = params.value("seconds", 0.0);
                if (params.contains("id")) s.id = params.value("id", s.id);
                if (params.contains("name")) s.name = params.value("name", s.name);
                s.first_use = params.value("firstUse", s.first_use);
            });
        } else if (event == "loadFailed") {
            const std::string raw = params.value("detail", "note model failed to load");
            const LoadFailure failure = ClassifyLoadFailure(raw);
            log::Printf("clinicavt-engine: note load failed (%.300s)%s\n", raw.c_str(),
                        failure == LoadFailure::kMemory ? Headroom().c_str() : "");
            Transition([&](NoteModelState& s) {
                s.phase = NoteModelState::Phase::kFailed;
                s.detail = failure == LoadFailure::kOther ? raw : PlainLoadMessage(failure, s.name);
                last_failure = failure;
                crashed_loading = false;
            });
        }
    }

    // Free commit, RAM and C: space, logged when a load fails for lack of memory
    static std::string Headroom() {
        MEMORYSTATUSEX memory{};
        memory.dwLength = sizeof(memory);
        ULARGE_INTEGER disk{};
        GlobalMemoryStatusEx(&memory);
        GetDiskFreeSpaceExW(L"C:\\", &disk, nullptr, nullptr);
        char text[160];
        std::snprintf(
            text, sizeof(text), "; commit free %.1f GB, RAM free %.1f GB, C: free %.1f GB",
            memory.ullAvailPageFile / 1e9, memory.ullAvailPhys / 1e9, disk.QuadPart / 1e9);
        return text;
    }

    // Memory failures are not retried automatically; only a tier switch or restart retries
    bool Blocked() const {
        std::lock_guard<std::mutex> lock(lane_mutex);
        return state.phase == NoteModelState::Phase::kFailed &&
               last_failure == LoadFailure::kMemory;
    }

    bool LoadFailed() const {
        std::lock_guard<std::mutex> lock(lane_mutex);
        return state.phase == NoteModelState::Phase::kFailed;
    }

    // A damaged compile cache fails every time and is the usual cause of a crash
    // during load. Rebuilt at most once per tier per engine run
    bool TakeCacheRebuild() {
        std::lock_guard<std::mutex> lock(lane_mutex);
        if (state.phase != NoteModelState::Phase::kFailed) return false;
        if (last_failure != LoadFailure::kCache && !crashed_loading) return false;
        if (std::find(cache_rebuilt.begin(), cache_rebuilt.end(), state.tier) !=
            cache_rebuilt.end()) {
            return false;
        }
        cache_rebuilt.push_back(state.tier);
        return true;
    }

    bool RebuildCache() {
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (closing) return false;
            if (!CloseWorker()) {
                Wedged();
                return false;
            }
        }
        if (store != nullptr) {
            const auto cache = models::CacheDir(store->Resolve("note", Tier()));
            std::error_code ignored;
            std::filesystem::remove_all(cache, ignored);
            log::Printf("clinicavt-engine: rebuilding the note model cache\n");
        }
        return true;
    }

    // Reads waiting frames and handles load events. Caller holds read_mutex. Stops
    // as soon as an attempt starts, leaving replies for it. False if the pipe is gone
    bool PumpFrames() {
        for (;;) {
            while (!attempt_active.load()) {
                const auto payload = pipe.NextFrame();
                if (!payload) break;
                const json message = json::parse(*payload, nullptr, false);
                if (message.is_object() && message.contains("method")) {
                    OnHostEvent(message["method"].get<std::string>(),
                                message.value("params", json::object()));
                } else if (message.is_object()) {
                    OnReply(message);
                }
            }
            if (attempt_active.load()) return true;
            switch (pipe.Read(4096)) {
                case ipc::PipeClient::Poll::kGone:
                    return false;
                case ipc::PipeClient::Poll::kNothing:
                    return true;
                case ipc::PipeClient::Poll::kRead:
                    break;
            }
        }
    }

    void StartWatcher() {
        StopWatcher();
        watch_stop = false;
        watcher = std::thread([this] {
            while (!watch_stop.load() && Phase() == NoteModelState::Phase::kLoading) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                if (attempt_active.load()) continue;
                std::lock_guard<std::mutex> reading(read_mutex);
                if (attempt_active.load()) continue;
                bool alive;
                {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    if (closing || respawning) return;
                    alive = WorkerAlive();
                }
                if (!alive || !PumpFrames()) {
                    // Only an unexpected host death is a failure; a deliberate close stops this
                    // thread first
                    std::lock_guard<std::mutex> lock(state_mutex);
                    if (closing || respawning) return;
                    const unsigned long code = host.ExitCode();
                    Transition([&](NoteModelState& s) {
                        if (s.phase != NoteModelState::Phase::kLoading) return;
                        s.phase = NoteModelState::Phase::kFailed;
                        s.detail = PlainLoadMessage(LoadFailure::kOther, s.name);
                        last_failure = LoadFailure::kOther;
                        crashed_loading = true;
                    });
                    log::Printf("clinicavt-engine: note host exited while loading (0x%lx)\n", code);
                    break;
                }
            }
            if (!watch_stop.load() && TakeCacheRebuild() && RebuildCache()) Prepare();
        });
    }

    void StopWatcher() {
        watch_stop = true;
        if (watcher.joinable()) {
            if (watcher.get_id() == std::this_thread::get_id()) {
                watcher.detach();
            } else {
                watcher.join();
            }
        }
    }

    // Bounded so a worker stuck in a driver call cannot block the note thread
    json ReadMessage() {
        const DWORD bound =
            Phase() == NoteModelState::Phase::kLoading ? kLoadTimeoutMs : kInactivityTimeoutMs;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(bound);
        for (;;) {
            {
                std::lock_guard<std::mutex> reading(read_mutex);
                if (auto payload = pipe.NextFrame()) {
                    return json::parse(*payload, nullptr, false);
                }
                switch (pipe.Read()) {
                    case ipc::PipeClient::Poll::kGone:
                        throw std::runtime_error("note worker died");
                    case ipc::PipeClient::Poll::kRead:
                        continue;
                    case ipc::PipeClient::Poll::kNothing:
                        break;
                }
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("note worker stopped responding");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    // Prefill acks pile up between attempts; drain them so the host's pipe writes
    // do not block. Only when no attempt is reading
    void DrainAcks() {
        std::lock_guard<std::mutex> reading(read_mutex);
        PumpFrames();
    }

    // One streamed attempt. Sends, then reads until the worker finishes it
    std::string Attempt(const std::string& method, const json& params, const Progress& progress) {
        struct ActiveFlag {
            std::atomic<bool>& flag;
            explicit ActiveFlag(std::atomic<bool>& f) : flag(f) {
                flag = true;
            }
            ~ActiveFlag() {
                flag = false;
            }
        } active{attempt_active};
        EnsureWorker();
        Send(method, params);
        for (;;) {
            const json message = ReadMessage();
            if (message.is_discarded() || !message.is_object()) {
                throw std::runtime_error("note worker spoke garbage");
            }
            if (message.contains("error")) {
                throw std::runtime_error(
                    message["error"].value("data", message["error"].value("message", "failed")));
            }
            if (!message.contains("method")) {
                OnReply(message);  // ack for this or an earlier prepare or prefill
                continue;
            }
            const auto& event = message["method"].get_ref<const std::string&>();
            const json& p = message.value("params", json::object());
            if (event == "partial" && progress) {
                progress(p.value("text", ""));
            } else if (event == "ready") {
                return p.value("text", "");
            } else if (event == "failed") {
                throw std::runtime_error(p.value("detail", "note generation failed"));
            } else {
                OnHostEvent(event, p);  // a load finished during the request
            }
        }
    }

    void Prepare() {
        if (Wedged() || Blocked()) return;
        try {
            EnsureWorker();
            bool starting = false;
            Transition([&starting](NoteModelState& s) {
                if (s.phase == NoteModelState::Phase::kReady) return;
                starting = s.phase != NoteModelState::Phase::kLoading;
                s.phase = NoteModelState::Phase::kLoading;
                s.detail.clear();
            });
            Send("prepare", json::object());
            if (starting) StartWatcher();
        } catch (const std::exception& e) {
            log::Printf("clinicavt-engine: note worker prepare failed (%s)\n", e.what());
            Transition([&e](NoteModelState& s) {
                s.phase = NoteModelState::Phase::kFailed;
                s.detail = e.what();
            });
        }
    }

    // Called when a GPU wait runs too long. Loads (minutes, uncancellable) and
    // in-flight requests (own timeout) are not probed. Otherwise ask the host to
    // exit. A healthy host exits within seconds and restarts on demand. One that
    // does not is stuck
    bool ProbeStuck() {
        if (Phase() == NoteModelState::Phase::kLoading || attempt_active.load()) return false;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (closing || !host.Alive()) return false;
            if (CloseWorker()) {
                Transition([](NoteModelState& s) {
                    if (s.phase == NoteModelState::Phase::kReady)
                        s.phase = NoteModelState::Phase::kIdle;
                });
                return false;
            }
        }
        Wedged();
        return true;
    }

    // True once a stuck host was found; marks the lane failed
    bool Wedged() {
        if (!system::GpuLease::Global().Wedged()) return false;
        Transition([](NoteModelState& s) {
            if (s.phase == NoteModelState::Phase::kFailed && s.detail == kStuckInDriver) return;
            s.phase = NoteModelState::Phase::kFailed;
            s.detail = kStuckInDriver;
        });
        return true;
    }

    // Retries once in a fresh process; that is the configuration measured to work
    std::string Run(const std::string& method, const json& params, const Progress& progress) {
        if (Wedged()) throw std::runtime_error(kStuckInDriver);
        if (Blocked()) throw std::runtime_error(State().detail);
        try {
            return Attempt(method, params, progress);
        } catch (const std::exception& e) {
            // The awaited load failed; a fresh process would fail too unless the cache was damaged
            if (LoadFailed()) {
                if (!TakeCacheRebuild() || !RebuildCache())
                    throw std::runtime_error(State().detail);
                return Attempt(method, params, progress);
            }
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                if (closing) throw;
                log::Printf("clinicavt-engine: note worker failed (%.100s); respawning\n",
                            e.what());
                respawning = true;
                if (!CloseWorker()) {
                    respawning = false;
                    Wedged();
                    throw std::runtime_error(kStuckInDriver);
                }
            }
            try {
                const std::string text = Attempt(method, params, progress);
                std::lock_guard<std::mutex> lock(state_mutex);
                respawning = false;
                return text;
            } catch (const std::exception& again) {
                {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    respawning = false;
                }
                Transition([&again](NoteModelState& s) {
                    s.phase = NoteModelState::Phase::kFailed;
                    s.detail = again.what();
                });
                throw;
            }
        }
    }
};

WorkerNoteWriter::WorkerNoteWriter(std::filesystem::path host_exe,
                                   std::filesystem::path models_root,
                                   std::filesystem::path prompt_path,
                                   const models::ModelStore* store, std::string tier,
                                   Listener listener)
    : impl_(new Impl{std::move(host_exe), std::move(models_root), std::move(prompt_path), store}) {
    impl_->listener = std::move(listener);
    impl_->state.tier = std::move(tier);
    try {
        impl_->Describe(impl_->state.tier, impl_->state);
    } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch) empty names
        // Nothing staged for the tier; state keeps empty names
    }
}

WorkerNoteWriter::~WorkerNoteWriter() {
    impl_->AwaitLoad();
    impl_->StopWatcher();
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    impl_->closing = true;
    impl_->CloseWorker();
}

// Spawn and load during capture; failures surface on Write
void WorkerNoteWriter::Prepare() {
    impl_->Prepare();
}

// A different tier spawns a new host. Same tier is a no-op unless its last load
// failed. Loads now so a failure shows in settings
NoteModelState WorkerNoteWriter::Configure(const std::string& tier) {
    NoteModelState described;
    described.tier = tier;
    try {
        impl_->Describe(tier, described);
    } catch (const std::exception& e) {
        throw std::invalid_argument(e.what());
    }
    bool same;
    bool loading;
    {
        std::lock_guard<std::mutex> lock(impl_->lane_mutex);
        same = impl_->state.tier == tier && impl_->state.phase != NoteModelState::Phase::kFailed;
        loading = impl_->state.phase == NoteModelState::Phase::kLoading;
    }
    if (same) {
        return State();
    }
    if (loading) {
        throw std::logic_error("the note model is still loading. Change it once it is ready");
    }
    impl_->StopWatcher();
    {
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        impl_->CloseWorker();
    }
    impl_->Transition([&](NoteModelState& s) {
        s = described;
        s.phase = NoteModelState::Phase::kIdle;
        impl_->last_failure = LoadFailure::kOther;
        impl_->crashed_loading = false;
    });
    Prepare();
    return State();
}

NoteModelState WorkerNoteWriter::State() const {
    std::lock_guard<std::mutex> lock(impl_->lane_mutex);
    return impl_->state;
}

void WorkerNoteWriter::Prefill(const std::vector<asr::Turn>& transcript,
                               const NoteOptions& options) {
    if (transcript.empty() || impl_->attempt_active.load() || impl_->Wedged()) return;
    try {
        impl_->EnsureWorker();
        impl_->DrainAcks();
        if (impl_->prefill_pending.load() != 0) return;  // the previous prefill is still running
        impl_->Send(
            "prefill",
            {{"turns", ipc::TurnsJson(transcript)}, {"style", NoteStyleName(options.style)}},
            &impl_->prefill_pending);
    } catch (const std::exception& e) {
        log::Printf("clinicavt-engine: note prefill not sent (%.100s)\n", e.what());
    }
}

std::string WorkerNoteWriter::Write(const std::vector<asr::Turn>& transcript,
                                    const NoteOptions& options, const Progress& progress) {
    if (transcript.empty()) {
        throw std::runtime_error("nothing to write: the transcript is empty");
    }
    return impl_->Run("write",
                      {{"turns", ipc::TurnsJson(transcript)},
                       {"style", NoteStyleName(options.style)},
                       {"detail", NoteDetailName(options.detail)},
                       {"confirmed", options.confirmed}},
                      progress);
}

std::string WorkerNoteWriter::WritePatient(const std::string& note, const Progress& progress) {
    if (note.empty()) {
        throw std::runtime_error("nothing to write: the note is empty");
    }
    return impl_->Run("writePatient", {{"note", note}}, progress);
}

std::string WorkerNoteWriter::WriteSummary(const std::string& note) {
    if (note.empty()) {
        throw std::runtime_error("nothing to summarise: the note is empty");
    }
    return impl_->Run("summary", {{"note", note}}, nullptr);
}

// On failure returns no title, without respawning or throwing
std::string WorkerNoteWriter::WriteLabel(const std::string& note) {
    if (note.empty()) {
        return {};
    }
    try {
        return impl_->Attempt("label", {{"note", note}}, nullptr);
    } catch (const std::exception& e) {
        log::Printf("clinicavt-engine: no label (%.100s)\n", e.what());
        return {};
    }
}

bool WorkerNoteWriter::CheckForStuckHost() {
    return impl_->ProbeStuck();
}

void WorkerNoteWriter::Cancel() {
    try {
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        if (impl_->WorkerAlive()) {
            impl_->Send("cancel", json::object());
        }
    } catch (...) {  // NOLINT(bugprone-empty-catch) cancel is best effort
    }
}

}  // namespace clinicavt::note
