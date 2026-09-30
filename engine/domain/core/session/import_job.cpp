#include "core/session/import_job.hpp"

#include <algorithm>
#include <exception>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace clinicavt::session {

int ImportPercent(ImportStage stage, double fraction) {
    struct Band {
        int from;
        int to;
    };
    static constexpr Band kBands[] = {{0, 5}, {5, 15}, {15, 95}, {95, 100}};
    const Band band = kBands[static_cast<int>(stage)];
    const double within = std::clamp(fraction, 0.0, 1.0) * (band.to - band.from);
    return band.from + static_cast<int>(within);
}

ImportJob::ImportJob(SessionState& state, Finaliser& finaliser, note::INoteWriter* note_writer,
                     metrics::Registry* metrics)
    : state_(state), finaliser_(finaliser), note_writer_(note_writer), metrics_(metrics) {}

ImportJob::~ImportJob() {
    Join();
}

void ImportJob::Launch(ImportRead read, ImportReport report) {
    cancel_ = false;
    thread_ = std::thread(
        [this, read = std::move(read), report = std::move(report)] { Run(read, report); });
}

void ImportJob::RequestCancel() {
    cancel_ = true;
}

void ImportJob::Join() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

// Catches everything. Frees the session slot before done, so a request made from done isn't
// refused as busy
void ImportJob::Run(const ImportRead& read, const ImportReport& report) {
    const store::SessionId id = state_.Current();
    const auto progress = [&report, &id](ImportStage stage, double fraction) {
        if (report.progress) report.progress(id, stage, ImportPercent(stage, fraction));
    };
    std::string error;
    try {
        std::vector<float> recording =
            read([&progress](double fraction) { progress(ImportStage::kReading, fraction); });
        {
            std::lock_guard<std::mutex> lock(state_.mutex);
            state_.session_audio = std::move(recording);
        }
        if (metrics_ != nullptr) {
            metrics_->BeginSession(true, 0.0);
        }
        if (note_writer_ != nullptr) {
            note_writer_->Prepare();
        }
        const ImportHooks hooks{cancel_, progress};
        // Imported audio may be another clinician, so don't update the voice print
        if (cancel_ || finaliser_.Finish(Outcome::kFinalise, false, &hooks) == Outcome::kCancel) {
            error = kImportCancelled;
        }
    } catch (const std::exception& e) {
        error = e.what();
    } catch (...) {
        error = "the recording could not be imported";
    }
    if (!error.empty()) {
        // Erase the session begun before the failed read
        try {
            finaliser_.Finish(Outcome::kCancel);
        } catch (...) {  // NOLINT(bugprone-empty-catch) the import error is reported
        }
    }
    {
        std::lock_guard<std::mutex> lock(state_.mutex);
        state_.running = false;
        state_.importing = false;
        cancel_ = false;
    }
    if (report.done) {
        try {
            report.done(id, error);
        } catch (...) {  // NOLINT(bugprone-empty-catch) the shell may already be gone
        }
    }
}

}  // namespace clinicavt::session
