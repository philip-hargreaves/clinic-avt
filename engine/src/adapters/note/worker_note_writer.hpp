#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "adapters/interfaces/note_tiers.hpp"
#include "ports/note_writer.hpp"

namespace clinicavt::models {
class ModelStore;
}  // namespace clinicavt::models

namespace clinicavt::note {

// Runs the note model in a supervised child process so a GPU driver fault cannot corrupt or hang
// the engine. A tier change starts a new host, so one model is resident
class WorkerNoteWriter : public INoteWriter, public INoteTiers {
   public:
    // store resolves tiers for Configure and names the model. Null, as in tests, keeps the tier
    // with no names. listener is called on every state transition, off the caller's thread
    WorkerNoteWriter(std::filesystem::path host_exe, std::filesystem::path models_root,
                     std::filesystem::path prompt_path, const models::ModelStore* store = nullptr,
                     std::string tier = "default", Listener listener = {});
    ~WorkerNoteWriter() override;

    void Prepare() override;

    // Fire-and-forget. Dropped while a generation is streaming
    void Prefill(const std::vector<asr::Turn>& transcript, const NoteOptions& options) override;

    bool WritesPatient() const override {
        return true;
    }

    std::string Write(const std::vector<asr::Turn>& transcript, const NoteOptions& options,
                      const Progress& progress) override;

    std::string WritePatient(const std::string& note, const Progress& progress) override;

    std::string WriteLabel(const std::string& note) override;
    std::string WriteSummary(const std::string& note) override;

    void Cancel() override;

    // True if the host holding the GPU is this lane's and will not exit. A healthy
    // host is closed and restarts on demand
    bool CheckForStuckHost();

    NoteModelState Configure(const std::string& tier) override;

    NoteModelState State() const override;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace clinicavt::note
