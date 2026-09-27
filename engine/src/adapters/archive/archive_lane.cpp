#include "adapters/archive/archive_lane.hpp"

#include <chrono>
#include <exception>
#include <optional>
#include <utility>

#include "adapters/archive/archive_password.hpp"

namespace clinicavt::archive {

namespace {

using nlohmann::json;

// Progress is for a bar, so a large backup does not send a notification per consultation
constexpr auto kProgressEvery = std::chrono::milliseconds(100);

const char* PhaseName(Phase phase) {
    switch (phase) {
        case Phase::kWriting:
            return "writing";
        case Phase::kChecking:
            return "checking";
        case Phase::kRestoring:
            return "restoring";
    }
    return "checking";
}

json DoneJson(const char* job, bool dry_run, std::size_t consultations, std::size_t reflections,
              std::size_t skipped, const Manifest& manifest, json ids) {
    return json{{"job", job},
                {"dryRun", dry_run},
                {"consultations", consultations},
                {"reflections", reflections},
                {"skipped", skipped},
                {"from", manifest.from},
                {"to", manifest.to},
                {"createdAt", manifest.created_at},
                {"ids", std::move(ids)}};
}

// Only fixed codes leave: an ArchiveError's own, and a failure to write for anything else, such
// as the store refusing a write
std::string CodeOf(std::exception_ptr failure) {
    try {
        std::rethrow_exception(failure);
    } catch (const ArchiveError& e) {
        return ArchiveError::Name(e.Code());
    } catch (...) {
        return ArchiveError::Name(ArchiveCode::kWriteFailed);
    }
}

}  // namespace

json ArchiveProgressJson(const std::string& job, Phase phase, std::size_t done, std::size_t total) {
    return json{{"job", job}, {"phase", PhaseName(phase)}, {"done", done}, {"total", total}};
}

json BackupDoneJson(const BackupResult& result) {
    return DoneJson("backup", false, result.manifest.consultations, result.reflections, 0,
                    result.manifest, result.ids);
}

json RestoreDoneJson(const RestoreResult& result, bool dry_run) {
    return DoneJson("restore", dry_run, result.Restored(), result.reflections, result.skipped,
                    result.manifest, json::array());
}

json ArchiveFailedJson(const std::string& job, const std::string& code) {
    return json{{"job", job}, {"code", code}};
}

ArchiveLane::ArchiveLane(store::ISessionStore& store, Emit emit, std::uint32_t iterations)
    : store_(store), emit_(std::move(emit)), iterations_(iterations) {}

ArchiveLane::~ArchiveLane() {
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        worker = std::move(worker_);
    }
    if (worker.joinable()) worker.join();
}

bool ArchiveLane::Launch(std::function<Outcome()> job) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_.load()) return false;
    if (worker_.joinable()) worker_.join();
    running_ = true;
    worker_ = std::thread([this, job = std::move(job)] {
        Outcome outcome = job();
        running_ = false;
        try {
            emit_(outcome.method, std::move(outcome.params));
        } catch (...) {  // NOLINT(bugprone-empty-catch) a shell that has gone reads it as over
        }
    });
    return true;
}

Progress ArchiveLane::Reporter(std::string job) {
    return [this, job = std::move(job), last = std::chrono::steady_clock::time_point{},
            last_phase = std::optional<Phase>()](Phase phase, std::size_t done,
                                                 std::size_t total) mutable {
        const auto now = std::chrono::steady_clock::now();
        if (phase == last_phase && done < total && now - last < kProgressEvery) return;
        last = now;
        last_phase = phase;
        emit_("archive/progress", ArchiveProgressJson(job, phase, done, total));
    };
}

bool ArchiveLane::BackUp(Period period, std::filesystem::path path, const std::string& password) {
    return Launch([this, period = std::move(period), path = std::move(path),
                   password = std::string(password)]() mutable -> Outcome {
        try {
            std::optional<ArchiveFileSink> sink;
            {
                WipeOnExit wipe{password};
                sink.emplace(path, password, iterations_);
            }
            return {"archive/done",
                    BackupDoneJson(archive::BackUp(store_, period, *sink, Reporter("backup")))};
        } catch (...) {
            return {"archive/failed",
                    ArchiveFailedJson("backup", CodeOf(std::current_exception()))};
        }
    });
}

bool ArchiveLane::Restore(std::filesystem::path path, const std::string& password, bool dry_run) {
    return Launch([this, path = std::move(path), password = std::string(password),
                   dry_run]() mutable -> Outcome {
        try {
            std::optional<ArchiveFileSource> source;
            {
                WipeOnExit wipe{password};
                source.emplace(path, password);
            }
            return {"archive/done",
                    RestoreDoneJson(archive::Restore(store_, *source, dry_run, Reporter("restore")),
                                    dry_run)};
        } catch (...) {
            return {"archive/failed",
                    ArchiveFailedJson("restore", CodeOf(std::current_exception()))};
        }
    });
}

}  // namespace clinicavt::archive
