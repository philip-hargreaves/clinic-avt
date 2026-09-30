#pragma once

#include <optional>
#include <string>
#include <vector>

#include "ports/reflection_codec.hpp"
#include "ports/session_store.hpp"

namespace clinicavt::records {

struct ReflectionEntry {
    Answers answers;  // references always set
    std::string created_at;
    std::string edited_at;
};

struct Reflection {
    std::string label;
    std::optional<store::Document> summary;  // scrubbed
    std::optional<ReflectionEntry> entry;
};

// Given parts replace the stored ones and omitted parts stay
struct ReflectionEdit {
    std::optional<std::string> happened;
    std::optional<std::string> learned;
    std::optional<std::string> next;
    std::optional<std::string> summary;
    std::optional<std::vector<Reference>> references;
};

// A row of the appraisal journal
struct ReflectionRow {
    store::SessionId id;
    std::string started_at;
    std::string label;
    Answers answers;
    std::string summary;  // scrubbed
    std::string created_at;
    std::string edited_at;
    bool sample = false;
};

// Appraisal reflections and case summaries on stored sessions. They are not part of the
// clinical record. Summaries are scrubbed on every read and write
class Reflections {
   public:
    Reflections(store::ISessionStore& sessions, const IReflectionCodec& codec)
        : sessions_(sessions), codec_(codec) {}

    // Throws for an unknown session
    Reflection Get(const store::SessionId& id);

    // Given answers and references replace stored ones and omitted ones stay. Only
    // reflection/delete removes one
    void Update(const store::SessionId& id, const ReflectionEdit& edit);

    // Removes the reflection and the summary
    void Delete(const store::SessionId& id);

    // Sessions with an appraisal entry, newest first as ListSessions returns them
    std::vector<ReflectionRow> List();

   private:
    store::ISessionStore& sessions_;
    const IReflectionCodec& codec_;
};

}  // namespace clinicavt::records
