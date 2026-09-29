#include "core/records/session_records.hpp"

#include <utility>

#include "ports/store_error.hpp"

namespace clinicavt::records {

std::vector<store::SessionSummary> SessionRecords::Consultations() {
    std::vector<store::SessionSummary> shown;
    for (auto& session : sessions_.ListSessions()) {
        if (session.cleared) continue;
        shown.push_back(std::move(session));
    }
    return shown;
}

std::size_t SessionRecords::DeleteAll(bool keep_reflections) {
    return sessions_.DeleteAll(keep_reflections);
}

std::size_t SessionRecords::Remove(const std::vector<store::SessionId>& ids,
                                   bool keep_reflections) {
    std::size_t removed = 0;
    for (const auto& id : ids) {
        try {
            if (keep_reflections) {
                sessions_.Clear(id);
            } else {
                sessions_.Delete(id);
            }
            removed += 1;
        } catch (const store::StoreError& e) {
            if (e.Code() != store::StoreCode::kNotFound) throw;
        }
    }
    return removed;
}

}  // namespace clinicavt::records
