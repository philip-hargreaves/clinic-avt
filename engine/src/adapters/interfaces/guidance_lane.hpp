#pragma once

#include <functional>
#include <string>

#include "adapters/interfaces/guidance_retriever.hpp"

namespace clinicavt::guidance {

struct SearchRequest {
    std::string session;   // empty for a typed query
    bool as_note = false;  // typed text searched the way a saved note is
    std::string note;
    int limit = 0;
    std::function<void(const Results&)> on_ready;
    std::function<void(const std::string& detail)> on_failed;
};

// One search at a time off the RPC thread. Note searches queue one per session in
// arrival order, ahead of a typed query (at most one). A new request for the same
// session (any session for typed queries) replaces the queued one, which fails
// with "superseded". Requests still queued at shutdown get no callback
class IGuidanceLane {
   public:
    virtual ~IGuidanceLane() = default;
    virtual void Prepare() = 0;
    virtual void Run(SearchRequest request) = 0;
};

}  // namespace clinicavt::guidance
