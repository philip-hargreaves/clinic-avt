#include "core/records/reflections.hpp"

#include <exception>
#include <utility>

#include "core/common/strings.hpp"
#include "core/note/summary_scrub.hpp"

namespace clinicavt::records {

using store::DocumentKind;

Reflection Reflections::Get(const store::SessionId& id) {
    Reflection result;
    result.label = sessions_.ReadDocument(id, DocumentKind::kLabel).text;
    // Scrub on read because stored text may predate the scrub or be hand-edited
    auto summary = sessions_.ReadDocument(id, DocumentKind::kSummary);
    if (!summary.text.empty()) {
        summary.text = note::ScrubSummary(summary.text);
        result.summary = std::move(summary);
    }
    const auto reflection = sessions_.ReadDocument(id, DocumentKind::kReflection);
    if (!reflection.text.empty()) {
        result.entry = ReflectionEntry{codec_.Decode(reflection.text), reflection.generated_at,
                                       reflection.edited_at};
    }
    return result;
}

void Reflections::Update(const store::SessionId& id, const ReflectionEdit& edit) {
    if (edit.summary) {
        sessions_.EditDocument(id, DocumentKind::kSummary,
                               note::ScrubSummary(strings::UnixLines(*edit.summary)));
    }
    const auto stored = sessions_.ReadDocument(id, DocumentKind::kReflection);
    Answers answers = codec_.Decode(stored.text);
    if (edit.happened) answers.happened = strings::UnixLines(*edit.happened);
    if (edit.learned) answers.learned = strings::UnixLines(*edit.learned);
    if (edit.next) answers.next = strings::UnixLines(*edit.next);
    if (edit.references) answers.references = *edit.references;
    if (stored.text.empty()) {
        store::Document document;
        document.text = codec_.Encode(answers);
        sessions_.SaveDocument(id, DocumentKind::kReflection, document);
    } else {
        sessions_.EditDocument(id, DocumentKind::kReflection, codec_.Encode(answers));
    }
}

void Reflections::Delete(const store::SessionId& id) {
    sessions_.DeleteDocument(id, DocumentKind::kReflection);
    sessions_.DeleteDocument(id, DocumentKind::kSummary);
    // A cleared consultation was kept only for its appraisal entry
    if (sessions_.Cleared(id)) sessions_.Delete(id);
}

std::vector<ReflectionRow> Reflections::List() {
    std::vector<ReflectionRow> rows;
    for (const auto& session : sessions_.ListSessions()) {
        if (!session.has_reflection) continue;
        try {
            const auto reflection = sessions_.ReadDocument(session.id, DocumentKind::kReflection);
            const auto summary = sessions_.ReadDocument(session.id, DocumentKind::kSummary);
            rows.push_back({.id = session.id,
                            .started_at = session.started_at,
                            .label = session.label,
                            .answers = codec_.Decode(reflection.text),
                            .summary = note::ScrubSummary(summary.text),
                            .created_at = reflection.generated_at.empty() ? summary.generated_at
                                                                          : reflection.generated_at,
                            .edited_at = reflection.edited_at,
                            .sample = session.sample});
        } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch) not listed
            // Skip sessions still recording or missing a key
        }
    }
    return rows;
}

}  // namespace clinicavt::records
