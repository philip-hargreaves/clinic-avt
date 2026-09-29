#include "core/session/note_lane.hpp"

#include <exception>
#include <stdexcept>
#include <utility>

#include "core/common/log.hpp"
#include "core/common/strings.hpp"
#include "core/note/note_gate.hpp"
#include "core/note/note_label.hpp"
#include "core/note/summary_scrub.hpp"

namespace clinicavt::session {
namespace {

std::size_t TranscriptWords(const std::vector<asr::Turn>& turns) {
    std::size_t words = 0;
    for (const auto& turn : turns) {
        words += static_cast<std::size_t>(strings::WordCount(turn.text));
    }
    return words;
}

}  // namespace

NoteLane::NoteLane(note::INoteWriter* writer, store::ISessionStore& store, ISessionEvents& events,
                   std::size_t min_note_words)
    : writer_(writer), store_(store), events_(events), min_note_words_(min_note_words) {}

NoteLane::~NoteLane() {
    Join();
}

bool NoteLane::Available() const {
    return writer_ != nullptr;
}

bool NoteLane::WritesPatient() const {
    return writer_ != nullptr && writer_->WritesPatient();
}

bool NoteLane::Busy() const {
    return busy_.load();
}

bool NoteLane::Refused() const {
    return refused_.load();
}

void NoteLane::ClearRefusal() {
    refused_ = false;
}

void NoteLane::SetOptions(note::NoteOptions options) {
    std::lock_guard<std::mutex> lock(mutex_);
    options_ = options;
}

note::NoteOptions NoteLane::Options() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return options_;
}

void NoteLane::WriteNote(store::SessionId id, std::vector<asr::Turn> transcript,
                         std::function<void()> accepted) {
    Join();
    const auto options = Options();
    if (const auto words = TranscriptWords(transcript); words < min_note_words_) {
        refused_ = true;
        events_.OnNoteRefused(std::to_string(words) + " words; a note needs at least " +
                                  std::to_string(min_note_words_),
                              false);
        return;
    }
    Run([this, id = std::move(id), turns = std::move(transcript), options,
         accepted = std::move(accepted)] {
        std::string note;
        try {
            note::RefusalFilter forward(
                [this](const std::string& partial) { events_.OnNotePartial(partial); });
            note = writer_->Write(turns, options,
                                  [&forward](const std::string& partial) { forward(partial); });
            if (const auto reason = note::RefusalReason(note);
                reason.has_value() && !options.confirmed) {
                log::Printf("clinicavt-engine: note refused: %s\n", reason->c_str());
                refused_ = true;
                events_.OnNoteRefused(*reason, true);
                return;  // no note, sheet or label, and accepted() is not called
            }
            if (const auto stored = SaveNote(id, note, options)) {
                // An OnNoteSaved handler failure must not lose the note
                try {
                    events_.OnNoteSaved(id, *stored);
                } catch (const std::exception& e) {
                    log::Printf("clinicavt-engine: work after the note not started: %s\n",
                                e.what());
                } catch (...) {
                    log::Printf("clinicavt-engine: work after the note not started\n");
                }
            }
            refused_ = false;
            events_.OnNoteReady(note);
            if (accepted) accepted();
        } catch (const std::exception& e) {
            events_.OnNoteFailed(e.what());
            return;
        } catch (...) {
            events_.OnNoteFailed("note generation failed");
            return;
        }
        if (writer_->WritesPatient() && !note.empty()) {
            WritePatientNow(id, note);
        }
        // Clear busy before the title so open and summary don't wait on it
        busy_ = false;
        if (!note.empty()) {
            SaveLabel(id, note);
        }
    });
}

void NoteLane::WritePatient(store::SessionId id, std::string note) {
    Run([this, id = std::move(id), note = std::move(note)] { WritePatientNow(id, note); });
}

void NoteLane::WriteSummary(store::SessionId id, std::string note) {
    Run([this, id = std::move(id), note = std::move(note)] {
        try {
            const std::string summary = note::ScrubSummary(writer_->WriteSummary(note));
            if (summary.empty()) {
                throw std::runtime_error("the model wrote nothing");
            }
            store::Document document;
            document.text = summary;
            store_.SaveDocument(id, store::DocumentKind::kSummary, document);
            events_.OnSummaryReady(id, summary);
        } catch (const std::exception& e) {
            events_.OnSummaryFailed(id, e.what());
        } catch (...) {
            events_.OnSummaryFailed(id, "case summary failed");
        }
    });
}

void NoteLane::Join() {
    std::thread thread;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        thread = std::move(thread_);
    }
    if (thread.joinable()) {
        abort_ = true;
        if (writer_ != nullptr) writer_->Cancel();
        thread.join();
        abort_ = false;
    }
}

// abort_ covers a Join that races the thread start. Without it the writer's per-generation cancel
// reset would drop the cancel
void NoteLane::Run(std::function<void()> work) {
    Join();
    std::lock_guard<std::mutex> lock(mutex_);
    busy_ = true;
    thread_ = std::thread([this, work = std::move(work)] {
        struct BusyGuard {
            std::atomic<bool>& flag;
            ~BusyGuard() {
                flag = false;
            }
        } busy_guard{busy_};
        if (abort_.load()) {
            return;
        }
        work();
    });
}

void NoteLane::WritePatientNow(const store::SessionId& id, const std::string& note) {
    try {
        const std::string patient = writer_->WritePatient(
            note, [this](const std::string& partial) { events_.OnPatientPartial(partial); });
        try {
            store::Document document;
            document.text = patient;
            store_.SaveDocument(id, store::DocumentKind::kPatient, document);
        } catch (const std::exception& e) {
            ReportStoreFailure(events_, "patient sheet", e);
        }
        events_.OnPatientReady(patient);
    } catch (const std::exception& e) {
        events_.OnPatientFailed(e.what());
    } catch (...) {
        events_.OnPatientFailed("patient information failed");
    }
}

// Returns the stored note, or nullopt if saving or reading it back failed. The caller still
// sends the text to the shell
std::optional<store::Document> NoteLane::SaveNote(const store::SessionId& id,
                                                  const std::string& text,
                                                  const note::NoteOptions& options) {
    store::Document document;
    document.text = text;
    document.style = note::NoteStyleName(options.style);
    document.detail = note::NoteDetailName(options.detail);
    try {
        store_.SaveDocument(id, store::DocumentKind::kNote, document);
    } catch (const std::exception& e) {
        ReportStoreFailure(events_, "note", e);
        return std::nullopt;
    }
    try {
        return store_.ReadDocument(id, store::DocumentKind::kNote);
    } catch (const std::exception& e) {
        ReportStoreFailure(events_, "note revision", e);
        return std::nullopt;
    }
}

// Never overwrites a user-edited label, and skips titles the sanitiser rejects
void NoteLane::SaveLabel(const store::SessionId& id, const std::string& note_text) {
    try {
        if (!store_.ReadDocument(id, store::DocumentKind::kLabel).edited_at.empty()) {
            return;
        }
        const std::string label = note::SanitiseLabel(writer_->WriteLabel(note_text));
        if (label.empty()) {
            return;
        }
        store::Document document;
        document.text = label;
        store_.SaveDocument(id, store::DocumentKind::kLabel, document);
    } catch (const std::exception& e) {
        ReportStoreFailure(events_, "label", e);
    }
}

}  // namespace clinicavt::session
