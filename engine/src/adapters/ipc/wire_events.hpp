#pragma once

#include <chrono>
#include <map>
#include <mutex>
#include <string>

#include "adapters/ipc/pipe_server.hpp"
#include "core/metrics/throughput.hpp"
#include "core/session/session_events.hpp"
#include "ports/guidance_lane.hpp"
#include "ports/session_store.hpp"
#include "ports/translator.hpp"

namespace clinicavt::ipc {

// Session events as pipe notifications. Streamed text is metered at the
// source and capped at ~12 Hz on the wire
class WireEvents : public session::ISessionEvents {
   public:
    WireEvents(PipeServer& server, store::ISessionStore& sessions);

    // The translator warms after the note. The note's guidance search starts
    // once the note is stored
    void SetTranslator(translate::ITranslator* translator);
    void SetGuidance(guidance::IGuidanceLane* lane);

    void OnLevel(const audio::LevelReading& reading) override;
    void OnInterrupted(audio::SourceEndReason reason, const std::string& detail) override;
    void OnProgress(const std::string& stage) override;
    void OnEnrolProgress(const audio::EnrolProgress& progress) override;
    void OnEnrolDone(bool ok, const std::string& detail, double speech_s) override;
    void OnNoteRefused(const std::string& reason, bool overridable) override;
    void OnNotePartial(const std::string& text) override;
    void OnNoteReady(const std::string& text) override;
    void OnNoteSaved(const std::string& session, const store::Document& note) override;
    void OnStorageFault(const std::string& detail) override;
    void OnNoteFailed(const std::string& detail) override;
    void OnPatientPartial(const std::string& text) override;
    void OnPatientReady(const std::string& text) override;
    void OnPatientFailed(const std::string& detail) override;
    void OnSummaryReady(const std::string& session, const std::string& text) override;
    void OnSummaryFailed(const std::string& session, const std::string& detail) override;

    // Translation streams through the same meter as the note
    void OnTranslation(const std::string& method, const nlohmann::json& params);

   private:
    void PushPartial(const std::string& method, nlohmann::json params);
    void PushStreamEnd(const char* partial_method, const char* method, nlohmann::json params);
    void DropStream(const char* partial_method);
    double Seconds(std::chrono::steady_clock::time_point now) const;

    PipeServer& server_;
    store::ISessionStore& sessions_;
    translate::ITranslator* translator_ = nullptr;
    guidance::IGuidanceLane* guidance_ = nullptr;
    std::mutex throttle_mutex_;
    std::map<std::string, std::chrono::steady_clock::time_point> last_partial_;
    std::map<std::string, metrics::ThroughputMeter> meters_;
    const std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
};

}  // namespace clinicavt::ipc
