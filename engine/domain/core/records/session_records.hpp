#pragma once

#include <cstddef>
#include <vector>

#include "ports/session_store.hpp"

namespace clinicavt::records {

class SessionRecords {
   public:
    explicit SessionRecords(store::ISessionCatalog& sessions) : sessions_(sessions) {}

    // Cleared sessions are appraisal entries. Only reflection/list shows them
    std::vector<store::SessionSummary> Consultations();

    // Crypto-erases every session. With keep_reflections, one with an appraisal entry is
    // cleared down to it instead. Returns the count
    std::size_t DeleteAll(bool keep_reflections);

    // Clears or erases each session as DeleteAll would. Ids already gone are not counted. Any other
    // store failure is thrown, and sessions already removed stay removed
    std::size_t Remove(const std::vector<store::SessionId>& ids, bool keep_reflections);

   private:
    store::ISessionCatalog& sessions_;
};

}  // namespace clinicavt::records
