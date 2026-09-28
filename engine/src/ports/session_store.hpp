#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "ports/store_error.hpp"
#include "ports/transcriber.hpp"

namespace clinicavt::store {

using SessionId = std::string;

struct SessionMeta {
    int sample_rate = 0;
    std::string device_id;
    std::string device_name;
    bool retain = true;      // false: erased once the consultation is left
    std::string started_at;  // ISO 8601 UTC, empty: now
};

struct SessionSummary {
    SessionId id;
    std::string started_at;  // ISO 8601 UTC
    std::string ended_at;    // Empty while recording or after a crash
    std::string state;       // recording | finalised
    int sample_rate = 0;
    std::string label;            // One-line summary, empty until a note exists
    std::string edited_at;        // Latest clinician edit to note or sheet, empty when none
    double audio_seconds = 0;     // From the sealed turns
    bool has_reflection = false;  // Has a summary or a reflection
    bool demo = false;            // A seeded sample rather than a real record
    bool cleared = false;         // Only the appraisal entry and label were kept
    std::string written_at;       // Latest write of any document, ISO 8601 UTC, empty when none
};

// Seeded session: finalised, given times, demo flag set
struct SessionSeed {
    std::string started_at;  // ISO 8601 UTC
    std::string ended_at;
    int sample_rate = 16000;
    std::vector<asr::Turn> turns;
};

// One text of each kind per finalised session; a rewrite replaces it. Summary and
// reflection are appraisal documents outside the clinical record. Guidance is
// what the note search showed
enum class DocumentKind { kNote, kPatient, kTranslation, kLabel, kSummary, kReflection, kGuidance };

// All kinds, for whole-session reads and writes
inline constexpr std::array kDocumentKinds{DocumentKind::kNote,        DocumentKind::kPatient,
                                           DocumentKind::kTranslation, DocumentKind::kLabel,
                                           DocumentKind::kSummary,     DocumentKind::kReflection,
                                           DocumentKind::kGuidance};

// Kinds kept when a session is cleared
inline constexpr std::array kKeptOnClear{DocumentKind::kLabel, DocumentKind::kSummary,
                                         DocumentKind::kReflection};

inline constexpr bool KeptOnClear(DocumentKind kind) {
    return std::ranges::find(kKeptOnClear, kind) != kKeptOnClear.end();
}

struct Document {
    std::string text;             // Empty when the session has no such document
    std::string language = "en";  // BCP 47
    std::string style;            // Note only: prose | soap
    std::string detail;           // Note only: concise | standard | detailed
    std::string generated_at;     // ISO 8601 UTC, when the model wrote it
    std::string edited_at;        // ISO 8601 UTC, empty until a person changed it
    std::int64_t revision = 0;    // Counts every write, 0 when absent
};

// One stored document, for moving a whole session
struct RecordDocument {
    DocumentKind kind = DocumentKind::kNote;
    Document document;  // revision kept as stored, so guidance staleness survives a move
};

// A finalised session without audio; one backup entry
struct SessionRecord {
    SessionId id;
    std::string started_at;
    std::string ended_at;
    int sample_rate = 0;
    std::string device_id;
    std::string device_name;
    std::uint64_t lost_frames = 0;
    std::vector<asr::Turn> turns;
    std::vector<RecordDocument> documents;  // one per kind present
};

enum class AddOutcome {
    kAdded,      // a new session
    kCompleted,  // a cleared session restored with what it had lost
    kSkipped,    // already stored, nothing written
};

// Audio is durable within a second, kept only to resume a crash. Finalise seals the
// transcript and erases audio, Cancel keeps nothing, anything else is a crash and
// stays discoverable
class ISessionStore {
   public:
    virtual ~ISessionStore() = default;

    virtual SessionId Begin(const SessionMeta& meta) = 0;

    // lost_frames: audio before these frames that never arrived (as in the audio port)
    virtual void Append(const SessionId& id, std::span<const float> frames,
                        std::uint64_t lost_frames) = 0;

    // Written at finalise in one transaction, before the session seals
    virtual void ReplaceTurns(const SessionId& id, std::span<const asr::Turn> turns) = 0;

    virtual void Finalise(const SessionId& id) = 0;
    virtual void Cancel(const SessionId& id) = 0;

    virtual void Abandon(const SessionId& id) = 0;

    virtual std::vector<SessionSummary> ListSessions() = 0;

    // Save stores a generation, Edit the clinician's text over it. Reading an
    // unknown session throws
    virtual void SaveDocument(const SessionId& id, DocumentKind kind, const Document& document) = 0;
    virtual void EditDocument(const SessionId& id, DocumentKind kind, const std::string& text) = 0;
    virtual Document ReadDocument(const SessionId& id, DocumentKind kind) = 0;

    // Removes one kind. A kind the session never had is not an error
    virtual void DeleteDocument(const SessionId& id, DocumentKind kind) = 0;

    // Read-back and disposal. All refuse the session currently recording
    virtual std::vector<asr::Turn> ReadTurns(const SessionId& id) = 0;

    // Stored capture in order, for resuming a crashed session
    virtual std::vector<float> ReadAudio(const SessionId& id) = 0;

    virtual void Delete(const SessionId& id) = 0;

    // Erases finalised retain-off sessions. Crashed ones are kept for recovery, so
    // the setting never loses audio
    virtual void EraseUnretained() = 0;

    // Seeded samples carry demo. ClearDemo removes only those
    virtual SessionId Seed(const SessionSeed& seed) = 0;
    virtual std::size_t ClearDemo() = 0;

    // Crypto-erases all sessions except one still recording. With keep_reflections,
    // sessions with an appraisal entry are cleared instead. Returns the count
    virtual std::size_t DeleteAll(bool keep_reflections = false) = 0;

    // Erases all of a finalised session except reflection, summary and label, so the
    // appraisal entry outlives the consultation. A session without one is deleted
    virtual void Clear(const SessionId& id) = 0;

    // For backup. Throws for an unknown or recording session
    virtual SessionRecord ReadRecord(const SessionId& id) = 0;

    // Adds a session from a backup in one transaction under a new key: finalised,
    // retained, not demo, timestamps and revisions as given. A cleared session with
    // the same id gets back its transcript and missing documents, keeping its own; any
    // other existing id is skipped. Throws for a record ValidRecord rejects
    virtual AddOutcome AddRecord(const SessionRecord& record) = 0;

    // Called off the caller thread when an audio commit fails; recording continues and retries
    // NOLINTNEXTLINE(performance-unnecessary-value-param) the store keeps the sink
    virtual void SetFaultListener(std::function<void(const StoreError&)>) {}
};

}  // namespace clinicavt::store
