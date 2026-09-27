#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "adapters/models/model_store.hpp"
#include "adapters/note/worker_note_writer.hpp"
#include "adapters/system/process_scan.hpp"

namespace clinicavt::note {
namespace {

const std::filesystem::path kModels = CLINICAVT_MODELS_DIR;

std::filesystem::path HostExe() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path().parent_path() / "clinicavt_note_host.exe";
}

std::filesystem::path PromptPath() {
    return kModels.parent_path() / "prompts";
}

// The OpenVINO debug GPU plugin asserts on a second generation
constexpr bool kDebugBuild =
#ifdef _DEBUG
    true;
#else
    false;
#endif

std::vector<asr::Turn> ElbowTranscript() {
    return {{0, 16000, "doctor", "What seems to be the problem today?"},
            {16000, 32000, "patient",
             "I noticed a swelling on my left elbow about a week ago. It is not painful, "
             "just slightly warm, and it feels like there is fluid inside."},
            {48000, 16000, "doctor", "Have you injured that elbow at all?"},
            {64000, 16000, "patient", "No, not that I know of."},
            {80000, 32000, "doctor",
             "This looks like bursitis. I would take ibuprofen, four hundred milligrams "
             "twice a day after food, and we will arrange blood tests."}};
}

// Every staged note tier through the real host, each on the pipeline its
// manifest names. CLINICAVT_SWEEP_TIER narrows it to one tier for the sweep;
// the host's own log carries verify, load and decode figures
TEST(WorkerNoteWriter, EveryStagedTierWritesANoteAndSheet) {
    if (!std::filesystem::exists(HostExe())) GTEST_SKIP() << "host not staged";
    if (kDebugBuild) GTEST_SKIP() << "OpenVINO 2026.3 debug GPU plugin asserts";
    char* wanted = nullptr;
    const std::string only =
        _dupenv_s(&wanted, nullptr, "CLINICAVT_SWEEP_TIER") == 0 && wanted != nullptr ? wanted : "";
    std::free(wanted);
    const models::ModelStore store(kModels);
    std::vector<std::string> tiers;
    for (const auto& model : store.List()) {
        if (model.task == "note" && (only.empty() || model.tier == only)) {
            tiers.push_back(model.tier);
        }
    }
    if (tiers.empty()) GTEST_SKIP() << "no note model staged";

    for (const auto& tier : tiers) {
        SCOPED_TRACE(tier);
        WorkerNoteWriter writer(HostExe(), kModels, PromptPath(), &store);
        const auto t0 = std::chrono::steady_clock::now();
        ASSERT_EQ(writer.Configure(tier).tier, tier);

        int partials = 0;
        const std::string note =
            writer.Write(ElbowTranscript(), {}, [&partials](const std::string&) { partials++; });
        const double to_note =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        ASSERT_FALSE(note.empty());
        EXPECT_GT(partials, 3) << "the note must stream through the pipe";
        EXPECT_EQ(note.find("doctor"), std::string::npos);

        const std::string sheet = writer.WritePatient(note, nullptr);
        EXPECT_NE(sheet.find("Your appointment today"), std::string::npos);
        EXPECT_NE(sheet.find("When to contact us"), std::string::npos);
        const auto state = writer.State();
        EXPECT_EQ(state.phase, NoteModelState::Phase::kReady);
        EXPECT_GT(state.seconds, 0.0) << "the host reported its load";
        std::fprintf(stderr,
                     "tier %s: %s, host load %.1f s, configure-to-note %.1f s, note %zu chars, "
                     "sheet %zu chars\n",
                     tier.c_str(), state.id.c_str(), state.seconds, to_note, note.size(),
                     sheet.size());
    }
}

// The pid of a note host that is not in `before`: the one this test started
DWORD NewNoteHost(const std::vector<DWORD>& before) {
    const auto now = system::ListProcesses();
    for (const auto& entry : now) {
        if (_wcsicmp(entry.image.c_str(), L"clinicavt_note_host.exe") != 0) continue;
        if (std::find(before.begin(), before.end(), entry.pid) == before.end()) return entry.pid;
    }
    return 0;
}

std::vector<DWORD> NoteHosts() {
    std::vector<DWORD> pids;
    for (const auto& entry : system::ListProcesses()) {
        if (_wcsicmp(entry.image.c_str(), L"clinicavt_note_host.exe") == 0)
            pids.push_back(entry.pid);
    }
    return pids;
}

TEST(WorkerNoteWriter, AKilledWorkerRespawnsAndTheNoteStillArrives) {
    if (!std::filesystem::exists(kModels / "qwen3.5-9b-int4") ||
        !std::filesystem::exists(HostExe())) {
        GTEST_SKIP() << "note model or host not staged";
    }
    if (kDebugBuild) GTEST_SKIP() << "OpenVINO 2026.3 debug GPU plugin asserts";
    const auto before = NoteHosts();
    WorkerNoteWriter writer(HostExe(), kModels, PromptPath());
    writer.Prepare();
    const DWORD mine = NewNoteHost(before);
    ASSERT_NE(mine, 0u);

    std::atomic<bool> killed{false};
    const std::string note =
        writer.Write(ElbowTranscript(), {}, [&killed, mine](const std::string& partial) {
            // The first streamed words prove generation is mid-flight, then this
            // test's own worker dies under it. Any other host on the machine is left alone
            if (partial.size() > 20 && !killed.exchange(true)) {
                HANDLE host = OpenProcess(PROCESS_TERMINATE, FALSE, mine);
                if (host != nullptr) {
                    TerminateProcess(host, 1);
                    CloseHandle(host);
                }
            }
        });

    EXPECT_TRUE(killed.load());
    ASSERT_FALSE(note.empty());
    EXPECT_EQ(note.find("doctor"), std::string::npos);
}

}  // namespace
}  // namespace clinicavt::note
