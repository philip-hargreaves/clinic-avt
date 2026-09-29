#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

#include "adapters/interfaces/guidance_lane.hpp"
#include "adapters/interfaces/guidance_retriever.hpp"

namespace clinicavt::guidance {

using ReadinessListener = std::function<void(const Readiness&)>;

// One worker over the retriever. Loads in the background and runs request
// callbacks on the worker, except the superseded failure, which runs on the
// caller thread. The listener is told how loading ended
class GuidanceLane : public IGuidanceLane {
   public:
    explicit GuidanceLane(IGuidanceRetriever& retriever, ReadinessListener on_readiness = {});
    ~GuidanceLane() override;

    void Prepare() override;
    void Run(SearchRequest request) override;

   private:
    void Start();  // under mutex_
    void Work();
    static void Fail(const SearchRequest& request, const char* detail);

    IGuidanceRetriever& retriever_;
    ReadinessListener on_readiness_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    bool prepare_ = false;
    bool stop_ = false;
    std::deque<SearchRequest> pending_notes_;  // one per session, in arrival order
    std::optional<SearchRequest> pending_text_;
};

}  // namespace clinicavt::guidance
