#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "ports/note_lane.hpp"
#include "ports/note_writer.hpp"

namespace clinicavt::models {
class ModelStore;
}  // namespace clinicavt::models

namespace clinicavt::note {

// The note model in its own supervised process, so a GPU driver fault can
// never corrupt or hang the engine. The process boundary is also the tier
// switch: a new tier is a new host, so exactly one model is ever resident
class WorkerNoteWriter : public INoteWriter, public INoteLane {
   public:
    // store: resolves tiers for Configure and names the model in the
    // state. Null leaves the lane on its tier with no names (tests)
    WorkerNoteWriter(std::filesystem::path host_exe, std::filesystem::path models_root,
                     std::filesystem::path prompt_path, const models::ModelStore* store = nullptr,
                     std::string tier = "default");
    ~WorkerNoteWriter() override;

    void Prepare() override;

    // Sent without waiting, and dropped while a generation is streaming
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

    // True when the host holding the GPU is this lane's and it will not exit.
    // A healthy host is closed and starts again when next needed
    bool CheckForStuckHost();

    NoteModelState Configure(const std::string& tier) override;

    NoteModelState State() const override;

    void SetListener(Listener listener) override;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace clinicavt::note
