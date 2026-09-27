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
    std::mutex read_mutex;      // one reader of the pipe at a time: the attempt or the watcher
    system::ChildProcess host;  // kill-on-close: the engine's death is the worker's
    std::vector<system::ChildProcess> stuck;  // hosts that would not exit, held so none is killed
    ipc::PipeClient pipe;
    std::int64_t next_id = 1;
    // Process-wide: a host winding down keeps its pipe name briefly, so a
    // second writer must not reuse it
    static inline std::atomic<int> spawn_count{0};
    bool closing = false;
    bool respawning = false;                  // under state_mutex: Run is between attempts
    std::atomic<bool> attempt_active{false};  // the note thread owns the pipe's read side
    // The prefill not yet answered, 0 for none. One at a time, so a host that
    // stops reading can never fill the pipe and block the capture thread
    std::atomic<std::int64_t> prefill_pending{0};

    // The lane: which tier, whether resident. lane_mutex is never held
    // across a call into the host or the listener
    mutable std::mutex lane_mutex;
    NoteModelState state;
    Listener listener;
    // Under lane_mutex: why the last load failed, and the tiers whose cache
    // has already been rebuilt once
    LoadFailure last_failure = LoadFailure::kOther;
    bool crashed_loading = false;
    std::vector<std::string> cache_rebuilt;
    std::thread watcher;  // reads the host's load outcome while nothing else reads
    std::atomic<bool> watch_stop{false};

    // A generation streams partials constantly, so this much silence means the
    // worker is wedged inside a driver call and only a respawn recovers it.
    // A request queued behind a load is silent for as long as the load
    // takes (hash + compile of a 19 GB model: minutes), so that wait has
    // its own, longer bound
    static constexpr DWORD kInactivityTimeoutMs = 120'000;
    static constexpr DWORD kLoadTimeoutMs = 20 * 60'000;
    // A host that has lost its pipe cancels any generation and exits. The
    // slowest measured exit is 3.4 s after an unfinished prefill
    static constexpr DWORD kExitGraceMs = 15'000;

    bool WorkerAlive() const {
        return host.Alive() && pipe.IsOpen();
    }

    // False when the host would not exit. It is then stuck in a driver call,
    // and is held rather than killed
    bool CloseWorker() {
        pipe.Close();
        prefill_pending = 0;
        if (host.End(kExitGraceMs)) return true;
        std::fprintf(stderr, "clinicavt-engine: note host %lu did not exit; leaving it\n",
                     host.Pid());
        stuck.push_back(std::move(host));
        host = {};
        system::GpuLease::Global().MarkWedged();
        return false;
    }

    // A load cannot be cancelled, so a host is only closed once its load has
    // settled. Bounded by the load's own timeout
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

    // Spawns the host and connects its private pipe. Throws when the host
    // cannot start, which surfaces as a failed note
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
        // The host claims the pipe before any model work, so the connect is quick
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

    // `pending` names the request before it goes, so its reply can never come first
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

    // A reply with no method answers a request. The prefill's frees the next
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

    // Names the model a tier resolves to, and whether its compile cache
    // exists. Throws the store's own message when nothing claims the tier
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

    // The host's two load outcomes, from whichever reader saw them
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
            std::fprintf(stderr, "clinicavt-engine: note load failed (%.300s)%s\n", raw.c_str(),
                         failure == LoadFailure::kMemory ? Headroom().c_str() : "");
            Transition([&](NoteModelState& s) {
                s.phase = NoteModelState::Phase::kFailed;
                s.detail = failure == LoadFailure::kOther ? raw : PlainLoadMessage(failure, s.name);
                last_failure = failure;
                crashed_loading = false;
            });
        }
    }

    // What the machine had left when a load was refused memory
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

    // A memory failure is not retried behind the user's back: only a switch
    // or a restart tries again
    bool Blocked() const {
        std::lock_guard<std::mutex> lock(lane_mutex);
        return state.phase == NoteModelState::Phase::kFailed &&
               last_failure == LoadFailure::kMemory;
    }

    bool LoadFailed() const {
        std::lock_guard<std::mutex> lock(lane_mutex);
        return state.phase == NoteModelState::Phase::kFailed;
    }

    // A damaged compile cache fails the same way every time, and a crash
    // during a load is most often one. Rebuilt once per tier per engine
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

    // Closes the host, deletes the tier's compile cache and loads again
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
            std::fprintf(stderr, "clinicavt-engine: rebuilding the note model cache\n");
        }
        return true;
    }

    // Reads whatever frames are waiting and dispatches load events. The
    // caller holds read_mutex. Stops the moment an attempt starts, leaving
    // its replies for it. False when the pipe is gone
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

    // Reads the host's load outcome while no attempt owns the pipe
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
                    // Only a host that died on its own is a failure. A
                    // deliberate close stopped this thread first
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
                    std::fprintf(
                        stderr, "clinicavt-engine: note host exited while loading (0x%lx)\n", code);
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

    // Bounded: a worker wedged inside a driver call must not wedge the
    // note thread with it
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

    // Prefill acks pile up unread between attempts. Draining them keeps the
    // host's pipe writes from blocking. Only when no attempt owns the reads
    void DrainAcks() {
        std::lock_guard<std::mutex> reading(read_mutex);
        PumpFrames();
    }

    // One streamed attempt: request, then read until the worker settles it
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
                OnReply(message);  // an ack, ours or an earlier prepare's or prefill's
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
                OnHostEvent(event, p);  // a load settling under the request
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
            std::fprintf(stderr, "clinicavt-engine: note worker prepare failed (%s)\n", e.what());
            Transition([&e](NoteModelState& s) {
                s.phase = NoteModelState::Phase::kFailed;
                s.detail = e.what();
            });
        }
    }

    // For a GPU wait that has run on too long. A load holds the GPU for minutes
    // and cannot be interrupted, and a request in flight has its own bound, so
    // neither is probed. Otherwise the host is asked to exit: a healthy one does
    // within seconds and starts again when next needed. One that cannot is stuck
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

    // True, and the lane marked failed, once a stuck host has been found
    bool Wedged() {
        if (!system::GpuLease::Global().Wedged()) return false;
        Transition([](NoteModelState& s) {
            if (s.phase == NoteModelState::Phase::kFailed && s.detail == kStuckInDriver) return;
            s.phase = NoteModelState::Phase::kFailed;
            s.detail = kStuckInDriver;
        });
        return true;
    }

    // One fresh process before failing: the fresh-context retry is the
    // configuration measured to work
    std::string Run(const std::string& method, json params, const Progress& progress) {
        if (Wedged()) throw std::runtime_error(kStuckInDriver);
        if (Blocked()) throw std::runtime_error(State().detail);
        try {
            return Attempt(method, params, progress);
        } catch (const std::exception& e) {
            // The request waited on a load that failed: a fresh process would
            // fail the same way, unless the cause was a damaged cache
            if (LoadFailed()) {
                if (!TakeCacheRebuild() || !RebuildCache())
                    throw std::runtime_error(State().detail);
                return Attempt(method, params, progress);
            }
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                if (closing) throw;
                std::fprintf(stderr, "clinicavt-engine: note worker failed (%.100s); respawning\n",
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
                                   const models::ModelStore* store, std::string tier)
    : impl_(new Impl{std::move(host_exe), std::move(models_root), std::move(prompt_path), store}) {
    impl_->state.tier = std::move(tier);
    try {
        impl_->Describe(impl_->state.tier, impl_->state);
    } catch (const std::exception&) {
        // Nothing staged for the tier: the state says so with empty names
    }
}

WorkerNoteWriter::~WorkerNoteWriter() {
    impl_->AwaitLoad();
    impl_->StopWatcher();
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    impl_->closing = true;
    impl_->CloseWorker();
}

// Spawn and load hide inside capture. Failure surfaces on Write
void WorkerNoteWriter::Prepare() {
    impl_->Prepare();
}

// A different tier is a new host. The same tier is a no-op unless its last
// load failed. Loads immediately so a failure surfaces at the setting
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

void WorkerNoteWriter::SetListener(Listener listener) {
    std::lock_guard<std::mutex> lock(impl_->lane_mutex);
    impl_->listener = std::move(listener);
}

void WorkerNoteWriter::Prefill(const std::vector<asr::Turn>& transcript,
                               const NoteOptions& options) {
    if (transcript.empty() || impl_->attempt_active.load() || impl_->Wedged()) return;
    try {
        impl_->EnsureWorker();
        impl_->DrainAcks();
        if (impl_->prefill_pending.load() != 0) return;  // the last guess is still running
        impl_->Send("prefill", {{"turns", ipc::TurnsJson(transcript)}, {"style", options.style}},
                    &impl_->prefill_pending);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "clinicavt-engine: note prefill not sent (%.100s)\n", e.what());
    }
}

std::string WorkerNoteWriter::Write(const std::vector<asr::Turn>& transcript,
                                    const NoteOptions& options, const Progress& progress) {
    if (transcript.empty()) {
        throw std::runtime_error("nothing to write: the transcript is empty");
    }
    return impl_->Run("write",
                      {{"turns", ipc::TurnsJson(transcript)},
                       {"style", options.style},
                       {"detail", options.detail},
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

// A failed title leaves the note without a title: no respawn, no throw
std::string WorkerNoteWriter::WriteLabel(const std::string& note) {
    if (note.empty()) {
        return {};
    }
    try {
        return impl_->Attempt("label", {{"note", note}}, nullptr);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "clinicavt-engine: no label (%.100s)\n", e.what());
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
    } catch (...) {  // NOLINT(bugprone-empty-catch)
    }
}

}  // namespace clinicavt::note
