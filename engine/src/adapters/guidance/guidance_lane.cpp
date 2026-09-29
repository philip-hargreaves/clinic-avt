#include "adapters/guidance/guidance_lane.hpp"

#include <algorithm>
#include <exception>
#include <utility>

#include "core/common/log.hpp"

namespace clinicavt::guidance {

GuidanceLane::GuidanceLane(IGuidanceRetriever& retriever, ReadinessListener on_readiness)
    : retriever_(retriever), on_readiness_(std::move(on_readiness)) {}

GuidanceLane::~GuidanceLane() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void GuidanceLane::Prepare() {
    std::lock_guard<std::mutex> lock(mutex_);
    prepare_ = true;
    Start();
    wake_.notify_all();
}

void GuidanceLane::Run(SearchRequest request) {
    std::optional<SearchRequest> displaced;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (request.session.empty()) {
            displaced =
                std::exchange(pending_text_, std::optional<SearchRequest>(std::move(request)));
        } else {
            const auto same = std::find_if(
                pending_notes_.begin(), pending_notes_.end(),
                [&](const SearchRequest& waiting) { return waiting.session == request.session; });
            if (same == pending_notes_.end()) {
                pending_notes_.push_back(std::move(request));
            } else {
                displaced = std::exchange(*same, std::move(request));
            }
        }
        Start();
    }
    wake_.notify_all();
    if (displaced) Fail(*displaced, "superseded");
}

void GuidanceLane::Start() {
    if (!worker_.joinable()) worker_ = std::thread([this] { Work(); });
}

// Catches callback exceptions so they cannot kill the worker or RPC thread
void GuidanceLane::Fail(const SearchRequest& request, const char* detail) {
    if (!request.on_failed) return;
    try {
        request.on_failed(detail);
    } catch (...) {
        log::Printf("clinicavt-engine: guidance failure not delivered: %s\n", detail);
    }
}

void GuidanceLane::Work() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        wake_.wait(
            lock, [this] { return stop_ || prepare_ || !pending_notes_.empty() || pending_text_; });
        if (stop_) return;
        if (prepare_) {
            prepare_ = false;
            lock.unlock();
            try {
                retriever_.Prepare();
                int available = 0, unavailable = 0;
                for (const auto& corpus : retriever_.Corpora()) {
                    ++(corpus.unavailable.empty() ? available : unavailable);
                }
                log::Printf("clinicavt-engine: guidance ready, %d corpora, %d unavailable\n",
                            available, unavailable);
            } catch (const std::exception& e) {
                log::Printf("clinicavt-engine: guidance unavailable: %s\n", e.what());
            }
            if (on_readiness_) on_readiness_(retriever_.Status());
            lock.lock();
            continue;
        }
        SearchRequest request;
        if (pending_notes_.empty()) {
            request = std::move(*pending_text_);
            pending_text_.reset();
        } else {
            request = std::move(pending_notes_.front());
            pending_notes_.pop_front();
        }
        lock.unlock();
        try {
            const bool note = !request.session.empty() || request.as_note;
            const Results results = retriever_.Search(
                request.note, request.limit, note ? SearchMode::kNote : SearchMode::kQuery);
            if (request.on_ready) request.on_ready(results);
        } catch (const std::exception& e) {
            Fail(request, e.what());
        } catch (...) {
            Fail(request, "guidance search failed");
        }
        lock.lock();
    }
}

}  // namespace clinicavt::guidance
