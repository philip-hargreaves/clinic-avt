#pragma once

#include <functional>
#include <span>
#include <vector>

#include "core/diarisation/resplit.hpp"
#include "core/diarisation/role_naming.hpp"
#include "core/diarisation/tidy_transcript.hpp"
#include "core/diarisation/turn_decode.hpp"
#include "core/metrics/metrics.hpp"
#include "ports/audio_source.hpp"
#include "ports/diariser.hpp"
#include "ports/session_events.hpp"
#include "ports/transcriber.hpp"

namespace clinicavt::session {

struct Transcript {
    std::vector<asr::Turn> turns;  // attributed and tidied, empty when nothing was heard
    diar::DiariseResult diarised;
    int doctor_cluster = -1;  // -1: no speaker was named the doctor
};

// Named finalise stages as they complete, for the log and the metrics
using StageFn = std::function<void(const char*)>;

// Diarises, decodes each merged turn (mostly from the capture cache), re-splits edge chunks by
// voice, names roles and tidies. Anchor similarities run on the CPU alongside the GPU decode
Transcript TranscribeRecording(std::span<const float> audio, diar::IDiariser& diariser,
                               asr::ITranscriber& transcriber, ICaptureEvents& events,
                               metrics::Registry* metrics, const StageFn& stage);

}  // namespace clinicavt::session
