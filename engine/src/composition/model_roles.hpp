#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "adapters/interfaces/note_tiers.hpp"
#include "ports/diariser.hpp"
#include "ports/streaming_vad.hpp"
#include "ports/transcriber.hpp"

namespace clinicavt::asr {
class WhisperTranscriber;
}  // namespace clinicavt::asr
namespace clinicavt::diar {
class AnchorStore;
}  // namespace clinicavt::diar
namespace clinicavt::metrics {
class Registry;
}  // namespace clinicavt::metrics
namespace clinicavt::models {
class ModelStore;
class OvRuntime;
}  // namespace clinicavt::models
namespace clinicavt::note {
class WorkerNoteWriter;
}  // namespace clinicavt::note
namespace clinicavt::translate {
class NllbTranslator;
}  // namespace clinicavt::translate

namespace clinicavt::composition {

// first_use: a compile cache was missing, so one-off compiles are running. missing: roles a
// consultation needs that are not installed
struct RoleReport {
    bool first_use = false;
    std::vector<std::string> missing;
};

// A role that cannot load uses a scripted stand-in under --scripted, as in CI. Otherwise it is
// added to missing and consultations are refused until it is installed

struct Transcriber {
    std::unique_ptr<asr::ITranscriber> port;
    asr::WhisperTranscriber* whisper = nullptr;  // null for a stand-in; used to switch device
};

Transcriber BuildTranscriber(const models::ModelStore& store, models::OvRuntime& runtime,
                             const std::string& device, metrics::Registry& metrics, bool scripted,
                             RoleReport& report);

// Compiles in the background; session/start waits for it, hello does not
std::unique_ptr<audio::IStreamingVad> BuildVad(const models::ModelStore& store,
                                               models::OvRuntime& runtime,
                                               metrics::Registry& metrics, bool scripted,
                                               RoleReport& report);

// Needs both the diarisation and segmentation models
std::unique_ptr<diar::IDiariser> BuildDiariser(const models::ModelStore& store,
                                               models::OvRuntime& runtime,
                                               diar::AnchorStore& anchors,
                                               metrics::Registry& metrics, bool scripted,
                                               RoleReport& report);

// The note tier "auto" means on this machine, chosen from its memory and logged. Empty when
// no note model is staged
std::string MachineNoteTier(const models::ModelStore& store);

// Note generation in a supervised child, so a GPU driver fault only costs a respawn.
// Null if nothing can write
std::unique_ptr<note::WorkerNoteWriter> BuildNoteWriter(models::ModelStore& store,
                                                        const std::filesystem::path& models_root,
                                                        const std::string& requested_tier,
                                                        const std::string& auto_tier,
                                                        note::INoteTiers::Listener listener,
                                                        RoleReport& report);

// CPU only, so no GPU contention. Null if the model is not staged
std::unique_ptr<translate::NllbTranslator> BuildTranslator(const models::ModelStore& store,
                                                           models::OvRuntime& runtime,
                                                           RoleReport& report);

// An orphaned note host exits within seconds; one still present after that is
// stuck in the driver until reboot. Checked before any model uses the GPU, and marks the
// GPU lease wedged when found
bool FindStrayNoteHost();

}  // namespace clinicavt::composition
