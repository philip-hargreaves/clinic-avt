#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ports/note_writer.hpp"

namespace clinicavt::models {
class ModelStore;
class OvRuntime;
}  // namespace clinicavt::models

namespace clinicavt::note {

// Qwen behind the note port: one background load, resident pipeline,
// prompts re-read per note. The tier is a role the store resolves. The
// manifest says which pipeline loads it
class LlmNoteWriter : public INoteWriter {
   public:
    // Outcome of one load, reported to the supervising process
    struct LoadReport {
        bool ok = false;
        std::string id;
        std::string name;
        std::string detail;      // the failure, when !ok
        double seconds = 0;      // verify + build, when ok
        bool first_use = false;  // no compile cache existed: a load that took minutes
    };

    using LoadListener = std::function<void(const LoadReport&)>;

    LlmNoteWriter(const models::ModelStore& store, models::OvRuntime& runtime,
                  std::filesystem::path prompt_dir, std::string tier = "default");
    ~LlmNoteWriter() override;

    std::string Write(const std::vector<asr::Turn>& transcript, const NoteOptions& options,
                      const Progress& progress) override;

    bool WritesPatient() const override {
        return true;
    }

    std::string WritePatient(const std::string& note, const Progress& progress) override;

    std::string WriteLabel(const std::string& note) override;
    std::string WriteSummary(const std::string& note) override;

    // Loads the pipeline in the background. Idempotent, retried on failure
    void Prepare() override;

    // One discarded token over the guessed prompt prefix. Skipped while a
    // generation runs, the model is still loading, the GPU is busy, or its
    // pipeline does not extend the KV. Only a driver fault is thrown
    void Prefill(const std::vector<asr::Turn>& transcript, const NoteOptions& options) override;

    void Cancel() override;

    // Cancels the running generation and every later one, for a host whose
    // engine has gone
    void Close();

    // Called from the loader thread when a load ends, either way
    void SetLoadListener(LoadListener listener);

    // Called every few seconds while a load or generation waits for the GPU,
    // with the seconds waited, so the engine knows the host is alive
    void SetGpuWaitListener(std::function<void(double)> listener);

   private:
    std::string Generate(const std::string& prompt, const Progress& progress,
                         std::size_t max_new_tokens = 1024);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace clinicavt::note
