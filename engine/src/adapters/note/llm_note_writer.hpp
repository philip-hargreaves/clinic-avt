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

// Qwen note writer: one background load, resident pipeline, prompts re-read per
// note. The tier is resolved by the model store; the manifest picks the pipeline
class LlmNoteWriter : public INoteWriter {
   public:
    struct LoadReport {
        bool ok = false;
        std::string id;
        std::string name;
        std::string detail;      // the failure, when !ok
        double seconds = 0;      // verify + build, when ok
        bool first_use = false;  // no compile cache existed, so the load took minutes
    };

    using LoadListener = std::function<void(const LoadReport&)>;

    LlmNoteWriter(const models::ModelStore& store, models::OvRuntime& runtime,
                  std::filesystem::path prompt_dir, std::string tier = "default",
                  // Called on the loader thread when a load succeeds or fails
                  LoadListener on_load = {},
                  // Called every few seconds while waiting for the GPU, with seconds waited, as a
                  // liveness signal to the engine
                  std::function<void(double)> on_gpu_wait = {});
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

    // Generates one discarded token over the guessed prompt prefix. Skipped while
    // generating, loading, the GPU is busy, or the pipeline cannot extend the KV.
    // Throws only on a driver fault
    void Prefill(const std::vector<asr::Turn>& transcript, const NoteOptions& options) override;

    void Cancel() override;

    // Cancels the running and all later generations; for when the engine is gone
    void Close();

   private:
    std::string Generate(const std::string& prompt, const Progress& progress,
                         std::size_t max_new_tokens = 1024);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace clinicavt::note
