#include "adapters/note/note_host_process.hpp"

#include <atomic>
#include <exception>
#include <stdexcept>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "adapters/system/gpu_lease.hpp"
#include "core/common/log.hpp"

namespace clinicavt::note {

namespace {

// Process-wide because an exiting host keeps its pipe name briefly
std::atomic<int> spawn_count{0};

}  // namespace

NoteHostProcess::NoteHostProcess(std::filesystem::path host_exe, std::filesystem::path models_root,
                                 std::filesystem::path prompt_path, system::GpuLease& gpu)
    : host_exe_(std::move(host_exe)),
      models_root_(std::move(models_root)),
      prompt_path_(std::move(prompt_path)),
      gpu_(gpu) {}

void NoteHostProcess::Spawn(const std::string& tier) {
    pipe_path_ = L"\\\\.\\pipe\\LOCAL\\clinicavt-note-" + std::to_wstring(GetCurrentProcessId()) +
                 L"-" + std::to_wstring(++spawn_count);
    const std::wstring args = L"\"" + pipe_path_ + L"\" \"" + models_root_.wstring() + L"\" \"" +
                              prompt_path_.wstring() + L"\" \"" +
                              std::wstring(tier.begin(), tier.end()) + L"\"";
    try {
        host_ = system::ChildProcess::Spawn(host_exe_, args, {.exempt_from_throttling = true});
    } catch (const std::exception&) {
        throw std::runtime_error("note worker failed to start");
    }
}

// The host creates the pipe before loading models, so connecting is fast
std::string NoteHostProcess::Connect() {
    for (int attempt = 0; attempt < 150; ++attempt) {
        if (pipe_.Open(pipe_path_)) break;
        if (host_.WaitFor(100)) break;  // died before serving
    }
    if (!pipe_.IsOpen()) {
        return "note worker pipe did not open";
    }
    if (pipe_.ServerPid() != host_.Pid()) {
        return "note worker pipe is not the spawned process";
    }
    return {};
}

bool NoteHostProcess::Alive() const {
    return host_.Alive();
}

bool NoteHostProcess::Connected() const {
    return host_.Alive() && pipe_.IsOpen();
}

unsigned long NoteHostProcess::Pid() const {
    return host_.Pid();
}

unsigned long NoteHostProcess::ExitCode() const {
    return host_.ExitCode();
}

ipc::PipeClient& NoteHostProcess::Pipe() {
    return pipe_;
}

void NoteHostProcess::ClosePipe() {
    pipe_.Close();
}

bool NoteHostProcess::End() {
    if (host_.End(kExitGraceMs)) return true;
    log::Printf("clinicavt-engine: note host %lu did not exit; leaving it\n", host_.Pid());
    stuck_.push_back(std::move(host_));
    host_ = {};
    gpu_.MarkWedged();
    return false;
}

}  // namespace clinicavt::note
