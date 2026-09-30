#pragma once

#include <atomic>
#include <thread>

#include "core/metrics/metrics.hpp"
#include "core/session/finaliser.hpp"
#include "core/session/import_progress.hpp"
#include "core/session/session_state.hpp"
#include "ports/note_writer.hpp"

namespace clinicavt::session {

// Reads an external recording and finalises it on its own thread, without updating the voice print
class ImportJob {
   public:
    ImportJob(SessionState& state, Finaliser& finaliser, note::INoteWriter* note_writer,
              metrics::Registry* metrics);
    ~ImportJob();
    ImportJob(const ImportJob&) = delete;
    ImportJob& operator=(const ImportJob&) = delete;

    // The session is already claimed and begun
    void Launch(ImportRead read, ImportReport report);
    // Caller holds state.mutex and has seen state.importing. The import stops at its next span
    void RequestCancel();
    void Join();

   private:
    void Run(const ImportRead& read, const ImportReport& report);

    SessionState& state_;
    Finaliser& finaliser_;
    note::INoteWriter* note_writer_;
    metrics::Registry* metrics_;
    // Started and joined on the RPC thread
    std::thread thread_;
    std::atomic<bool> cancel_{false};
};

}  // namespace clinicavt::session
