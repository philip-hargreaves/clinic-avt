#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ports/transcriber.hpp"

namespace clinicavt::diar {

// Takes a clip and its absolute first frame and returns Whisper chunks in absolute frames. The text
// is their concatenation
using DecodeClipFn = std::function<std::vector<asr::Turn>(std::span<const float>, std::uint64_t)>;
// Returns true to abort a catch-up. It is checked between steps
using StopFn = std::function<bool()>;

// Exact decode span -> speculated text
using TurnTexts = std::map<std::pair<std::uint64_t, std::uint64_t>, std::string>;
// Exact decode span -> that decode's chunks
using TurnChunks = std::map<std::pair<std::uint64_t, std::uint64_t>, std::vector<asr::Turn>>;

struct LabelledSlice {
    std::uint64_t first_frame = 0;
    std::uint64_t end_frame = 0;
    int cluster = 0;
};

// Where a finalise-time Diarise spent its time, for measurement only
struct DiariseTiming {
    double finish_s = 0;  // VAD tail + segmenting the un-segmented remainder
    double embed_s = 0;   // per-slice embeddings not served by the capture cache
    int embed_hits = 0;
    int embed_misses = 0;
    double cluster_s = 0;
    double overlap_s = 0;  // second-opinion embeds for overlap spans
};

struct DiariseResult {
    std::vector<LabelledSlice> slices;  // time-sorted
    int cluster_count = 0;
    DiariseTiming timing;
};

// Capture-phase work. Without it Diarise processes the whole recording
class ICaptureDiarisation {
   public:
    virtual ~ICaptureDiarisation() = default;

    virtual void Advance(std::span<const float> audio, const DecodeClipFn& decode) = 0;

    // The finalise catch-up. It runs Advance with no budget, so every settled span is decoded and
    // cut before Diarise. A stop can leave it partial
    virtual void Settle(std::span<const float> audio, const DecodeClipFn& decode,
                        const StopFn& stop) = 0;

    // Runs only Settle's speech-finding step, with progress, for imports. Settle then only decodes
    virtual void FindSpeech(std::span<const float> audio,
                            const std::function<void(double)>& progress, const StopFn& stop) = 0;

    // Turn texts speculated by Advance, keyed on exact decode spans. Valid
    // after Diarise
    virtual TurnTexts TakeTurnTexts() = 0;

    // The chunks behind TakeTurnTexts, same keys, valid after Diarise
    virtual TurnChunks TakeTurnChunks() = 0;

    // The provisional opening of the sealed transcript, made of settled turns with cached text and
    // provisional roles. Valid after Advance, on its thread. Empty if nothing settled
    virtual std::vector<asr::Turn> SpeculativeTranscript() = 0;

    // Adds cut points from Whisper chunk edges, in absolute frames, for the next Advance and
    // Diarise
    virtual void AddCutPoints(std::span<const std::uint64_t> cuts) = 0;

    // Drop capture state a finalise will never consume (cancel, abandon)
    virtual void DiscardCapture() = 0;
};

// Speaker embeddings for the re-split and the clinician's voice anchor
class IVoiceprints {
   public:
    virtual ~IVoiceprints() = default;

    // Cluster centroids (unit norm), valid after Diarise. The re-split compares
    // edge chunks with them
    virtual std::vector<std::vector<float>> ClusterCentroids() = 0;

    // Embedding of [first, end) of the session audio, unit norm, empty when
    // too short. Served from the capture-phase cache where it has the span
    virtual std::vector<float> EmbedSpan(std::span<const float> audio, std::uint64_t first,
                                         std::uint64_t end) = 0;

    // Voiceprint of the speech (unit norm). Empty if the speech is too short or there is no
    // embedder. ReplaceAnchor turns an enrolment into the anchor
    virtual std::vector<float> EmbedVoice(std::span<const float> speech) = 0;
    virtual void ReplaceAnchor(std::span<const float> voiceprint, std::uint64_t enrolled_at) = 0;

    // The doctor cluster's voiceprint and the anchor update from it are separate calls, so the
    // update can wait until the note lane confirms it was a consultation
    virtual std::vector<float> DoctorVoiceprint(std::span<const float> audio,
                                                const std::vector<LabelledSlice>& slices,
                                                int doctor_cluster) = 0;
    virtual void AccrueVoiceprint(std::span<const float> voiceprint) = 0;
};

// Returns slices with anonymous labels plus anchor similarities. The caller assigns roles
class IDiariser {
   public:
    virtual ~IDiariser() = default;

    virtual DiariseResult Diarise(std::span<const float> audio) = 0;

    // Separate from Diarise so voiceprint embeds can overlap other work. DoctorVoiceprint reuses
    // them, so the doctor's print is embedded once
    virtual std::vector<double> AnchorSimilarities(std::span<const float> audio,
                                                   const std::vector<LabelledSlice>& slices,
                                                   int cluster_count) = 0;

    // Null when the diariser does not support it
    virtual ICaptureDiarisation* Capture() = 0;
    virtual IVoiceprints* Voiceprints() = 0;
};

}  // namespace clinicavt::diar
