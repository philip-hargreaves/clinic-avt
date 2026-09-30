#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>

#include "adapters/interfaces/guidance_lane.hpp"
#include "adapters/interfaces/guidance_retriever.hpp"
#include "core/common/worker_thread.hpp"

namespace clinicavt::guidance {

using ReadinessListener = std::function<void(const Readiness&)>;

// Runs the retriever on one worker thread. Loads in the background and runs request callbacks on
// the worker, except the superseded failure, which runs on the caller thread. The listener is told
// how loading ended
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
    WorkerThread worker_{mutex_, wake_};
    bool prepare_ = false;
    std::deque<SearchRequest> pending_notes_;  // one per session, in arrival order
    std::optional<SearchRequest> pending_text_;
};

}  // namespace clinicavt::guidance
