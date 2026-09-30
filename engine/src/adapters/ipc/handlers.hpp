#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "adapters/audio/capture_devices.hpp"
#include "adapters/diarisation/anchor_store.hpp"
#include "adapters/interfaces/document_ingest.hpp"
#include "adapters/interfaces/guidance_lane.hpp"
#include "adapters/interfaces/note_tiers.hpp"
#include "adapters/interfaces/recording_reader.hpp"
#include "adapters/ipc/messages.hpp"
#include "adapters/ipc/pipe_server.hpp"
#include "adapters/models/model_store.hpp"
#include "core/demo/sample_year.hpp"
#include "core/records/reflections.hpp"
#include "core/records/session_records.hpp"
#include "core/session/session_controller.hpp"

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

// All staged models; `active` marks the one each role loads (note: by configured tier)
json HandleModels(const clinicavt::models::ModelStore& models,
                  const std::string& note_tier = "default");

json NoteModelJson(const clinicavt::note::NoteModelState& state);

// note/tier: "auto" is the machine default. Refused during a consultation. An unknown or unstaged
// tier is an invalid-params error listing what is staged
std::variant<json, Error> HandleNoteTier(clinicavt::note::INoteTiers* lane, bool session_active,
                                         const json& params, const std::string& auto_tier = "");

json HandleAudioInputs(const std::vector<clinicavt::audio::CaptureDevice>& devices);

// Voiceprint source and consultation count. Clearing is refused during a session
json HandleAnchorStatus(const clinicavt::diar::AnchorStore& anchors);
std::variant<json, Error> HandleAnchorClear(clinicavt::diar::AnchorStore& anchors,
                                            bool session_active);

json HandleSessionList(clinicavt::records::SessionRecords& records);

std::variant<json, Error> HandleSessionNote(clinicavt::store::IDocumentStore& documents,
                                            const json& params);

std::variant<json, Error> HandleSessionPatient(clinicavt::store::IDocumentStore& documents,
                                               const json& params);

std::variant<json, Error> HandleSessionTranscript(clinicavt::store::ISessionCatalog& sessions,
                                                  const json& params);

std::variant<json, Error> HandleReflectionGet(clinicavt::records::Reflections& reflections,
                                              const json& params);
std::variant<json, Error> HandleReflectionUpdate(clinicavt::records::Reflections& reflections,
                                                 const json& params);
std::variant<json, Error> HandleReflectionDelete(clinicavt::records::Reflections& reflections,
                                                 const json& params);
json HandleReflectionList(clinicavt::records::Reflections& reflections);
std::variant<json, Error> HandleSessionDelete(clinicavt::store::ISessionCatalog& sessions,
                                              const json& params);
// Crypto-erases everything; refused while recording or during a backup. Without
// deleteReflections, a session with an appraisal entry is cleared down to it
std::variant<json, Error> HandleSessionDeleteAll(clinicavt::records::SessionRecords& records,
                                                 const json& params, bool session_active,
                                                 bool archive_busy);
// session/remove: clears or erases the given sessions like delete-all, once a
// backup holds them. Ids already gone are not counted
std::variant<json, Error> HandleSessionRemove(clinicavt::records::SessionRecords& records,
                                              const json& params, bool archive_busy);

// archive/summary: contents of a backup of the period, and how many consultations
// the last backup (covered) misses. archive/backup and archive/restore start a
// lane job; refused while recording or another job runs
std::variant<json, Error> HandleArchiveSummary(clinicavt::store::ISessionCatalog& sessions,
                                               const json& params);
std::variant<json, Error> HandleArchiveBackup(clinicavt::archive::ArchiveLane& lane,
                                              bool session_active, const json& params);
std::variant<json, Error> HandleArchiveRestore(clinicavt::archive::ArchiveLane& lane,
                                               bool session_active, const json& params);

// Seeds demo data unless it is already seeded. Clearing keeps real sessions
std::variant<json, Error> HandleDemoSeed(clinicavt::demo::DemoSamples& demo);
json HandleDemoClear(clinicavt::demo::DemoSamples& demo);

// Guidance panel shows the top three
inline constexpr int kGuidanceLimit = 3;
using Notify = std::function<void(const std::string& method, json params)>;
inline Notify PushTo(PipeServer& server) {
    return [&server](const std::string& method, json params) {
        server.PushNotification(method, std::move(params));
    };
}

// recording/inspect: length and date for the import dialog; stores nothing
std::variant<json, Error> HandleRecordingInspect(clinicavt::audio::IRecordingReader& reader,
                                                 const json& params);
// Plain-words message naming the missing roles a consultation needs. session/start,
// session/import and anchor/enrol refuse with it. Empty when none is missing
std::string MissingModelsReason(const std::vector<std::string>& missing);
// engine/readiness: ready once every staged model's compile cache exists. firstUse: the one-off
// compiles are running. strayNoteHost: a note host from an earlier engine is wedged in the GPU
// driver. missing: the roles a consultation needs that are not installed
json ReadinessJson(bool first_use, bool ready, bool stray_note_host,
                   const std::vector<std::string>& missing);
// session/start: starts a microphone session, or with replay plays a wav through the same
// pipeline if the engine allows replay. Refused while a role is missing
std::variant<json, Error> HandleSessionStart(clinicavt::session::SessionController& controller,
                                             clinicavt::translate::ITranslator* translator,
                                             const json& params,
                                             const std::vector<std::string>& missing,
                                             bool allow_replay);
// session/import: replies with the new session id, then finalises on the import
// thread, sending session/importProgress then session/imported or
// session/importFailed. Refused like session/start and for unreadable files.
// Errors never include the path
std::variant<json, Error> HandleSessionImport(clinicavt::audio::IRecordingReader& reader,
                                              clinicavt::session::SessionController& controller,
                                              clinicavt::translate::ITranslator* translator,
                                              const Notify& push, const json& params,
                                              const std::vector<std::string>& missing = {});
// session/importProgress: stage plus one overall percentage
json ImportProgressJson(const std::string& id, clinicavt::session::ImportStage stage, int percent);

// guidance/corpora: embedder state (loading, ready, unavailable) and every corpus
// dir. guidance/model sends only the state once loading ends
json GuidanceCorporaJson(const clinicavt::guidance::Readiness& readiness,
                         const std::vector<clinicavt::guidance::Corpus>& corpora);
json GuidanceModelJson(const clinicavt::guidance::Readiness& readiness);
// Lane request for a search: results as guidance/ready, failure as
// guidance/failed, both with the session (null for free text). With a session,
// the record is stored first and the payload says whether the note changed. If
// the session was erased meanwhile, nothing is sent; other store errors go in
// the payload
clinicavt::guidance::SearchRequest GuidanceSearchRequest(
    clinicavt::store::IDocumentStore& documents, const std::string& session,
    clinicavt::store::Document note, int limit, const Notify& notify);
// session/guidance: stored record, null if never searched or unreadable. stale:
// the note changed since. documentsChanged: the searched added documents differ
// from those ready now
std::variant<json, Error> HandleSessionGuidance(
    clinicavt::store::IDocumentStore& documents, const json& params,
    clinicavt::guidance::IDocumentIngest* ingest = nullptr);
// guidance/search: searches a session's stored note or free text via the lane.
// Replies immediately; results come as a notification
std::variant<json, Error> HandleGuidanceSearch(clinicavt::store::IDocumentStore& documents,
                                               clinicavt::guidance::IGuidanceLane& lane,
                                               const json& params, const Notify& notify);
// Added-document row used by guidance/documents, guidance/document and progress
json DocumentJson(const clinicavt::guidance::DocumentInfo& document);
json ProgressJson(const clinicavt::guidance::IngestProgress& progress);
// True when a document became ready or a ready one was removed
bool ChangesReadySet(const clinicavt::guidance::DocumentInfo& document);
// guidance/documents: folder and documents. guidance/documents/add copies files
// in and returns their rows, with skipped ones and reasons.
// guidance/documents/remove sends a document's files to the Recycle Bin
std::variant<json, Error> HandleDocumentsAdd(clinicavt::guidance::IDocumentIngest& ingest,
                                             const json& params);
std::variant<json, Error> HandleDocumentsList(clinicavt::guidance::IDocumentIngest& ingest);
std::variant<json, Error> HandleDocumentsRemove(clinicavt::guidance::IDocumentIngest& ingest,
                                                const json& params);
// guidance/page: renders one page of an added PDF to a bitmap in the scratch
// folder, with the cited chunk's boxes. guidance/documents/open: path of the
// file for the shell to open
std::variant<json, Error> HandleDocumentsPage(clinicavt::guidance::IDocumentIngest& ingest,
                                              const json& params);
std::variant<json, Error> HandleDocumentsOpen(clinicavt::guidance::IDocumentIngest& ingest,
                                              const json& params);
void RegisterGuidanceMethods(PipeServer& server, clinicavt::store::IDocumentStore& documents,
                             clinicavt::guidance::IGuidanceRetriever& retriever,
                             clinicavt::guidance::IGuidanceLane& lane,
                             clinicavt::guidance::IDocumentIngest& ingest);

// Reloads speech recognition on "GPU" or "NPU". `done` gets the error or "" once
// the load settles. Returns false if it cannot switch
using AsrSwitch =
    std::function<bool(const std::string& device, std::function<void(const std::string&)> done)>;

// asr/device: refused during a session. Replies "loading"; an asr/device
// notification reports ready or failed
std::variant<json, Error> HandleAsrDevice(const AsrSwitch& switcher, bool session_active,
                                          const json& params, std::function<void(json)> notify);

// Dependencies for the RPC methods. The references are always set and the rest only when staged.
// first_use is set when model caches were cold at launch, so one-off compiles are running and
// readiness reports them
struct EngineServices {
    clinicavt::session::SessionController& controller;
    const clinicavt::models::ModelStore& models;
    clinicavt::store::IDocumentStore& documents;
    clinicavt::store::ISessionCatalog& catalog;
    clinicavt::records::SessionRecords& records;
    clinicavt::records::Reflections& reflections;
    clinicavt::demo::DemoSamples& demo;
    clinicavt::metrics::Registry* metrics = nullptr;
    clinicavt::models::OvRuntime* runtime = nullptr;
    clinicavt::translate::ITranslator* translator = nullptr;
    clinicavt::translate::TranslateLane* translate_lane = nullptr;
    bool first_use = false;
    clinicavt::diar::AnchorStore* anchors = nullptr;
    clinicavt::note::INoteTiers* note_tiers = nullptr;
    std::string auto_note_tier;    // the note tier "auto" stands for on this machine
    bool stray_note_host = false;  // a note host from an earlier engine is stuck in the GPU driver
    AsrSwitch switch_asr;
    clinicavt::archive::ArchiveLane* archive_lane =
        nullptr;  // deletes are refused while a job runs
    clinicavt::audio::IRecordingReader* recordings = nullptr;  // null disables import
    std::vector<std::string> missing_models;  // roles not installed, empty with stand-ins
    bool allow_replay = false;                // session/start may play a file, for tests only
};

// engine/*, note/tier, anchor/* and audio/inputs
void RegisterEngineMethods(PipeServer& server, const EngineServices& services);
// session/*, note/* and patient/* edits and regeneration
void RegisterSessionMethods(PipeServer& server, const EngineServices& services);
// translate/languages and patient/translate, with a translator present
void RegisterTranslateMethods(PipeServer& server, const EngineServices& services);
// reflection/* and demo/*
void RegisterReflectionMethods(PipeServer& server, const EngineServices& services);
// recording/inspect and session/import, with a recording reader present
void RegisterImportMethods(PipeServer& server, const EngineServices& services);
// archive/*, with the archive lane present
void RegisterArchiveMethods(PipeServer& server, const EngineServices& services);

inline void RegisterMethods(PipeServer& server, const EngineServices& services) {
    RegisterEngineMethods(server, services);
    RegisterSessionMethods(server, services);
    RegisterTranslateMethods(server, services);
    RegisterReflectionMethods(server, services);
    RegisterImportMethods(server, services);
    if (services.archive_lane != nullptr) RegisterArchiveMethods(server, services);
}

}  // namespace clinicavt::ipc
