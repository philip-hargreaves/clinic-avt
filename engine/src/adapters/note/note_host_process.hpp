#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "adapters/ipc/pipe_client.hpp"
#include "adapters/system/child_process.hpp"

namespace clinicavt::system {
class GpuLease;
}  // namespace clinicavt::system

namespace clinicavt::note {

// Not thread-safe. WorkerNoteWriter calls Spawn, Connect and the close steps under state_mutex,
// and uses Pipe under read_mutex and write_mutex
class NoteHostProcess {
   public:
    NoteHostProcess(std::filesystem::path host_exe, std::filesystem::path models_root,
                    std::filesystem::path prompt_path, system::GpuLease& gpu);

    // Throws if the host cannot start, which shows as a failed note
    void Spawn(const std::string& tier);
    // Empty on success, else why the pipe cannot be used. The host is then left for the caller to
    // close
    std::string Connect();

    bool Alive() const;
    bool Connected() const;
    unsigned long Pid() const;
    unsigned long ExitCode() const;
    ipc::PipeClient& Pipe();

    void ClosePipe();
    // False if the host would not exit (stuck in a driver call)
    bool End();

   private:
    // A host that loses its pipe cancels and exits. The slowest measured exit was 3.4 s, after an
    // unfinished prefill
    static constexpr unsigned long kExitGraceMs = 15'000;

    std::filesystem::path host_exe_;
    std::filesystem::path models_root_;
    std::filesystem::path prompt_path_;
    system::GpuLease& gpu_;
    system::ChildProcess host_;                // in a kill-on-close job, so it dies with the engine
    std::vector<system::ChildProcess> stuck_;  // hosts that would not exit, held so none is killed
    ipc::PipeClient pipe_;
    std::wstring pipe_path_;
};

}  // namespace clinicavt::note
