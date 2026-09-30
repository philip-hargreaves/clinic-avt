#include "adapters/ipc/wire_events.hpp"

#include <cmath>
#include <utility>

#include "adapters/ipc/handlers.hpp"

namespace clinicavt::ipc {
namespace {

const char* ReasonName(audio::SourceEndReason reason) {
    return reason == audio::SourceEndReason::kDeviceLost ? "deviceLost" : "failed";
}

double Rounded(double rate) {
    return std::round(rate * 10.0) / 10.0;
}

}  // namespace

WireEvents::WireEvents(PipeServer& server, store::IDocumentStore& documents,
                       translate::ITranslator* translator, guidance::IGuidanceLane* guidance)
    : server_(server), documents_(documents), translator_(translator), guidance_(guidance) {}

void WireEvents::OnLevel(const audio::LevelReading& reading) {
    server_.PushNotification("audio.level",
                             {{"level", reading.level}, {"clipped", reading.clipped}});
}

void WireEvents::OnInterrupted(audio::SourceEndReason reason, const std::string& detail) {
    server_.PushNotification("session/interrupted",
                             {{"reason", ReasonName(reason)}, {"detail", detail}});
}

void WireEvents::OnProgress(const std::string& stage) {
    server_.PushNotification("session/progress", {{"stage", stage}});
}

void WireEvents::OnEnrolProgress(const audio::EnrolProgress& progress) {
    server_.PushNotification("anchor/progress", {{"elapsed", progress.elapsed_s},
                                                 {"speech", progress.speech_s},
                                                 {"level", progress.level.level},
                                                 {"clipped", progress.level.clipped}});
}

void WireEvents::OnEnrolDone(bool ok, const std::string& detail, double speech_s) {
    server_.PushNotification("anchor/enrolled",
                             {{"ok", ok}, {"detail", detail}, {"speechSeconds", speech_s}});
}

void WireEvents::OnNoteRefused(const std::string& reason, bool overridable) {
    server_.PushNotification("note/refused", {{"reason", reason}, {"overridable", overridable}});
}

void WireEvents::OnNotePartial(const std::string& text) {
    PushPartial("note/partial", {{"text", text}});
}

void WireEvents::OnNoteReady(const std::string& text) {
    PushStreamEnd("note/partial", "note/ready", {{"text", text}});
    // Warm up during the patient sheet write so the first translation isn't slower
    if (translator_ != nullptr) {
        translator_->Prepare();
    }
}

void WireEvents::OnNoteSaved(const std::string& session, const store::Document& note) {
    if (guidance_ == nullptr) return;
    guidance_->Run(
        GuidanceSearchRequest(documents_, session, note, kGuidanceLimit, PushTo(server_)));
}

void WireEvents::OnStorageFault(const std::string& detail) {
    server_.PushNotification("storage/fault", {{"detail", detail}});
}

void WireEvents::OnNoteFailed(const std::string& detail) {
    DropStream("note/partial");
    server_.PushNotification("note/failed", {{"detail", detail}});
}

void WireEvents::OnPatientPartial(const std::string& text) {
    PushPartial("patient/partial", {{"text", text}});
}

void WireEvents::OnPatientReady(const std::string& text) {
    PushStreamEnd("patient/partial", "patient/ready", {{"text", text}});
}

void WireEvents::OnPatientFailed(const std::string& detail) {
    DropStream("patient/partial");
    server_.PushNotification("patient/failed", {{"detail", detail}});
}

void WireEvents::OnSummaryReady(const std::string& session, const std::string& text) {
    server_.PushNotification("reflection/summary", {{"id", session}, {"text", text}});
}

void WireEvents::OnSummaryFailed(const std::string& session, const std::string& detail) {
    server_.PushNotification("reflection/summaryFailed", {{"id", session}, {"detail", detail}});
}

void WireEvents::OnTranslation(const std::string& method, const nlohmann::json& params) {
    if (method == "translate/partial") {
        PushPartial(method, params);
    } else if (method == "translate/ready") {
        PushStreamEnd("translate/partial", "translate/ready", params);
    } else {
        if (method == "translate/failed") DropStream("translate/partial");
        server_.PushNotification(method, params);
    }
}

// Metered before the notification cap so tokensPerSecond is the model rate
void WireEvents::PushPartial(const std::string& method, nlohmann::json params) {
    const auto now = std::chrono::steady_clock::now();
    double rate = 0;
    {
        std::lock_guard<std::mutex> lock(throttle_mutex_);
        auto& meter = meters_[method];
        meter.Token(Seconds(now));
        rate = meter.Rate(Seconds(now));
        auto& last = last_partial_[method];
        if (now - last < std::chrono::milliseconds(80)) {
            return;
        }
        last = now;
    }
    params["tokensPerSecond"] = Rounded(rate);
    server_.PushNotification(method, std::move(params));
}

// The end event carries the whole-generation average and resets the meter
void WireEvents::PushStreamEnd(const char* partial_method, const char* method,
                               nlohmann::json params) {
    double average = 0;
    {
        std::lock_guard<std::mutex> lock(throttle_mutex_);
        average = meters_[partial_method].Average();
        meters_.erase(partial_method);
        last_partial_.erase(partial_method);
    }
    if (average > 0) {
        params["tokensPerSecond"] = Rounded(average);
    }
    server_.PushNotification(method, std::move(params));
}

void WireEvents::DropStream(const char* partial_method) {
    std::lock_guard<std::mutex> lock(throttle_mutex_);
    meters_.erase(partial_method);
    last_partial_.erase(partial_method);
}

double WireEvents::Seconds(std::chrono::steady_clock::time_point now) const {
    return std::chrono::duration<double>(now - started_).count();
}

}  // namespace clinicavt::ipc
