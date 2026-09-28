#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <variant>

#include "adapters/audio/capture_devices.hpp"
#include "adapters/diarisation/anchor_store.hpp"
#include "adapters/ipc/messages.hpp"
#include "adapters/ipc/pipe_server.hpp"
#include "adapters/models/model_store.hpp"
#include "core/session/session_controller.hpp"
#include "ports/document_ingest.hpp"
#include "ports/guidance_lane.hpp"
#include "ports/note_lane.hpp"
#include "ports/recording_reader.hpp"

namespace clinicavt::models {
class OvRuntime;
}  // namespace clinicavt::models

namespace clinicavt::translate {
class ITranslator;
class TranslateLane;
}  // namespace clinicavt::translate

namespace clinicavt::archive {
class ArchiveLane;
}  // namespace clinicavt::archive

namespace clinicavt::ipc {

std::variant<json, Error> HandleHello(const json& params);

std::variant<json, Error> HandleEcho(const json& params);

// Every staged model. `active` marks the one each role loads, the note
// role by its configured tier
json HandleModels(const clinicavt::models::ModelStore& models,
                  const std::string& note_tier = "default");

json NoteModelJson(const clinicavt::note::NoteModelState& state);

// note/tier: the shell names a tier, the lane resolves and loads it. "auto" is
// this machine's pick. Refused during a consultation. An unknown or unstaged
// tier is a parameter error naming what is staged
std::variant<json, Error> HandleNoteTier(clinicavt::note::INoteLane* lane, bool session_active,
                                         const json& params, const std::string& auto_tier = "");

json HandleAudioInputs(const std::vector<clinicavt::audio::CaptureDevice>& devices);

// The clinician's voiceprint: where it came from and how many consultations
// refined it. Clearing is refused while a session runs
json HandleAnchorStatus(const clinicavt::diar::AnchorStore& anchors);
std::variant<json, Error> HandleAnchorClear(clinicavt::diar::AnchorStore& anchors,
                                            bool session_active);

json HandleSessionList(clinicavt::store::ISessionStore& sessions);

std::variant<json, Error> HandleSessionNote(clinicavt::store::ISessionStore& sessions,
                                            const json& params);

std::variant<json, Error> HandleSessionPatient(clinicavt::store::ISessionStore& sessions,
                                               const json& params);

std::variant<json, Error> HandleSessionTranscript(clinicavt::store::ISessionStore& sessions,
                                                  const json& params);

// Appraisal reflections on a stored session
std::variant<json, Error> HandleReflectionGet(clinicavt::store::ISessionStore& sessions,
                                              const json& params);
std::variant<json, Error> HandleReflectionUpdate(clinicavt::store::ISessionStore& sessions,
                                                 const json& params);
std::variant<json, Error> HandleReflectionDelete(clinicavt::store::ISessionStore& sessions,
                                                 const json& params);
json HandleReflectionList(clinicavt::store::ISessionStore& sessions);
std::variant<json, Error> HandleSessionDelete(clinicavt::store::ISessionStore& sessions,
                                              const json& params);
// One crypto-erase of everything stored, refused while a consultation records or a backup
// runs. Without deleteReflections a session with an appraisal entry is cleared to it instead
std::variant<json, Error> HandleSessionDeleteAll(clinicavt::store::ISessionStore& sessions,
                                                 const json& params, bool session_active,
                                                 bool archive_busy);
// session/remove: clears or erases the given sessions as Delete all does, once a backup holds
// them. An id already gone is not counted
std::variant<json, Error> HandleSessionRemove(clinicavt::store::ISessionStore& sessions,
                                              const json& params, bool archive_busy);

// archive/summary: what a backup of the period would hold, and how many consultations the last
// backup (covered) does not. archive/backup and archive/restore start a job on the lane,
// refused while a consultation records or another job runs
std::variant<json, Error> HandleArchiveSummary(clinicavt::store::ISessionStore& sessions,
                                               const json& params);
std::variant<json, Error> HandleArchiveBackup(clinicavt::archive::ArchiveLane& lane,
                                              bool session_active, const json& params);
std::variant<json, Error> HandleArchiveRestore(clinicavt::archive::ArchiveLane& lane,
                                               bool session_active, const json& params);

// Seed data from demo_dir, a no-op while present. Clearing leaves real sessions untouched
std::variant<json, Error> HandleDemoSeed(clinicavt::store::ISessionStore& sessions,
                                         const std::filesystem::path& demo_dir);
json HandleDemoClear(clinicavt::store::ISessionStore& sessions);

// Guidance: the panel shows the top three
inline constexpr int kGuidanceLimit = 3;
using Notify = std::function<void(const std::string& method, json params)>;
inline Notify PushTo(PipeServer& server) {
    return [&server](const std::string& method, json params) {
        server.PushNotification(method, std::move(params));
    };
}

// recording/inspect: a recording's length and date for the import dialog. Nothing is kept
std::variant<json, Error> HandleRecordingInspect(clinicavt::audio::IRecordingReader& reader,
                                                 const json& params);
// session/import: answers the new session's id at once, then finalises it on the import's
// thread, pushing session/importProgress and session/imported or session/importFailed from there.
// Refused as session/start is, and for a file the reader cannot open. No error carries the path
std::variant<json, Error> HandleSessionImport(clinicavt::audio::IRecordingReader& reader,
                                              clinicavt::session::SessionController& controller,
                                              clinicavt::translate::ITranslator* translator,
                                              const Notify& push, const json& params);
// session/importProgress: the import's stage and its one percentage across all stages
json ImportProgressJson(const std::string& id, clinicavt::session::ImportStage stage, int percent);

// guidance/corpora: whether the embedder is loading, ready or unavailable, and
// every corpus directory. guidance/model carries the state alone once loading ends
json GuidanceCorporaJson(const clinicavt::guidance::Readiness& readiness,
                         const std::vector<clinicavt::guidance::Corpus>& corpora);
json GuidanceModelJson(const clinicavt::guidance::Readiness& readiness);
// The lane request behind every search: results go out as guidance/ready, a
// failure as guidance/failed, both naming the session (null for free text).
// With a session the record is stored first and the payload says whether the
// note moved. A session erased meanwhile ends the search quietly, any other
// store error rides on the payload
clinicavt::guidance::SearchRequest GuidanceSearchRequest(clinicavt::store::ISessionStore& sessions,
                                                         const std::string& session,
                                                         clinicavt::store::Document note, int limit,
                                                         Notify notify);
// session/guidance: the stored record, null when the note was never searched
// or the record cannot be read, stale when the note has been written since
// documentsChanged: the added documents the record searched are not the ones ready now
std::variant<json, Error> HandleSessionGuidance(
    clinicavt::store::ISessionStore& sessions, const json& params,
    clinicavt::guidance::IDocumentIngest* ingest = nullptr);
// guidance/search: the stored note of session id, or free text, through the
// lane. The reply is immediate. The results arrive as a notification
std::variant<json, Error> HandleGuidanceSearch(clinicavt::store::ISessionStore& sessions,
                                               clinicavt::guidance::IGuidanceLane& lane,
                                               const json& params, const Notify& notify);
// Added documents: the row guidance/documents lists and guidance/document
// announces, and the progress notification
json DocumentJson(const clinicavt::guidance::DocumentInfo& document);
json ProgressJson(const clinicavt::guidance::IngestProgress& progress);
// The ready set changes when a document finishes or a finished one goes
bool ChangesReadySet(const clinicavt::guidance::DocumentInfo& document);
// guidance/documents: the folder and its documents. guidance/documents/add
// copies files into the folder and answers with their rows, the rest skipped
// with a reason. guidance/documents/remove sends a document's files to the
// Recycle Bin
std::variant<json, Error> HandleDocumentsAdd(clinicavt::guidance::IDocumentIngest& ingest,
                                             const json& params);
std::variant<json, Error> HandleDocumentsList(clinicavt::guidance::IDocumentIngest& ingest);
std::variant<json, Error> HandleDocumentsRemove(clinicavt::guidance::IDocumentIngest& ingest,
                                                const json& params);
// guidance/page: one page of an added PDF drawn to a bitmap under the scratch
// folder, with the cited chunk's boxes. guidance/documents/open: the file in
// the guidelines folder, for the shell to hand to a viewer
std::variant<json, Error> HandleDocumentsPage(clinicavt::guidance::IDocumentIngest& ingest,
                                              const json& params);
std::variant<json, Error> HandleDocumentsOpen(clinicavt::guidance::IDocumentIngest& ingest,
                                              const json& params);
void RegisterGuidanceMethods(PipeServer& server, clinicavt::store::ISessionStore& sessions,
                             clinicavt::guidance::IGuidanceRetriever& retriever,
                             clinicavt::guidance::IGuidanceLane& lane,
                             clinicavt::guidance::IDocumentIngest& ingest);

// Moves speech recognition to another device ("GPU" or "NPU") in place. `done`
// runs once the load settles, with the error or an empty string. False when it
// cannot move
using AsrSwitch =
    std::function<bool(const std::string& device, std::function<void(const std::string&)> done)>;

// asr/device: refused during a session. The reply says loading, and an
// asr/device notification says ready or failed once the load settles
std::variant<json, Error> HandleAsrDevice(const AsrSwitch& switcher, bool session_active,
                                          const json& params, std::function<void(json)> notify);

// Everything the methods reach. The controller, models and store are always
// present. The rest is wired when its model or feature is staged. first_use:
// model caches were cold at launch, so the one-off compiles are running and
// readiness reports them
struct EngineServices {
    clinicavt::session::SessionController& controller;
    const clinicavt::models::ModelStore& models;
    clinicavt::store::ISessionStore& sessions;
    clinicavt::metrics::Registry* metrics = nullptr;
    clinicavt::models::OvRuntime* runtime = nullptr;
    clinicavt::translate::ITranslator* translator = nullptr;
    clinicavt::translate::TranslateLane* translate_lane = nullptr;
    bool first_use = false;
    clinicavt::diar::AnchorStore* anchors = nullptr;
    clinicavt::note::INoteLane* note_lane = nullptr;
    std::string auto_note_tier;    // the note tier "auto" stands for on this machine
    bool stray_note_host = false;  // one from an earlier engine is wedged in the GPU driver
    std::filesystem::path demo_dir;
    AsrSwitch switch_asr;
    clinicavt::archive::ArchiveLane* archive_lane = nullptr;   // deletes are refused while it runs
    clinicavt::audio::IRecordingReader* recordings = nullptr;  // import is absent without it
};

// engine/*, note/tier, anchor/* and audio/inputs
void RegisterEngineMethods(PipeServer& server, const EngineServices& services);
// session/*, note/*, patient/*, reflection/*, demo/* and translate/*
void RegisterSessionMethods(PipeServer& server, const EngineServices& services);
// archive/*, with the archive lane present
void RegisterArchiveMethods(PipeServer& server, const EngineServices& services);

inline void RegisterMethods(PipeServer& server, const EngineServices& services) {
    RegisterEngineMethods(server, services);
    RegisterSessionMethods(server, services);
    if (services.archive_lane != nullptr) RegisterArchiveMethods(server, services);
}

}  // namespace clinicavt::ipc
