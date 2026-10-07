#include "adapters/guidance/guidance_lane.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace clinicavt::guidance {
namespace {

using namespace std::chrono_literals;

// Searches record their note and can be held open until released
struct FakeRetriever : IGuidanceRetriever {
    std::mutex mutex;
    std::condition_variable changed;
    bool hold = false;
    bool prepare_throws = false;
    bool search_throws = false;
    int prepares = 0;
    std::vector<std::string> searched;
    std::vector<SearchMode> modes;

    void Prepare() override {
        std::lock_guard<std::mutex> lock(mutex);
        ++prepares;
        changed.notify_all();
        if (prepare_throws) throw std::runtime_error("no embedding model staged");
    }
    Results Search(const std::string& note, int limit, SearchMode mode) override {
        std::unique_lock<std::mutex> lock(mutex);
        if (prepare_throws) throw std::runtime_error("no embedding model staged");
        if (search_throws) throw std::runtime_error("corpus gone");
        searched.push_back(note);
        modes.push_back(mode);
        changed.notify_all();
        changed.wait(lock, [this] { return !hold; });
        Results results;
        results.considered = limit;
        Result one;
        one.chunk_id = note;
        results.shown.push_back(one);
        return results;
    }
    std::vector<Corpus> Corpora() override {
        return {};
    }
    Readiness Status() override {
        std::lock_guard<std::mutex> lock(mutex);
        if (prepare_throws) return {Readiness::Phase::kUnavailable, "no embedding model staged"};
        return {Readiness::Phase::kReady, ""};
    }

    template <typename Pred>
    bool WaitUntil(Pred pred) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, 5s, pred);
    }
    void Release() {
        std::lock_guard<std::mutex> lock(mutex);
        hold = false;
        changed.notify_all();
    }
};

// Callback results, with wait helpers
struct Outcome {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::string> ready;   // first chunk id of each result set
    std::vector<std::string> failed;  // details
    std::thread::id last_thread;

    SearchRequest Request(std::string note, int limit = 3, std::string session = "") {
        SearchRequest request;
        request.session = std::move(session);
        request.note = std::move(note);
        request.limit = limit;
        request.on_ready = [this](const Results& results) {
            std::lock_guard<std::mutex> lock(mutex);
            ready.push_back(results.shown.empty() ? "" : results.shown[0].chunk_id);
            last_thread = std::this_thread::get_id();
            changed.notify_all();
        };
        request.on_failed = [this](const std::string& detail) {
            std::lock_guard<std::mutex> lock(mutex);
            failed.push_back(detail);
            changed.notify_all();
        };
        return request;
    }
    template <typename Pred>
    bool WaitUntil(Pred pred) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, 5s, pred);
    }
};

// One typed slot and one per session. A newer request replaces the one still
// waiting in its slot, and waiting notes run before typed text
TEST(GuidanceLane, OnlyTheLatestWaitingSearchOfEachKindRunsAndNotesGoFirst) {
    FakeRetriever retriever;
    retriever.hold = true;
    Outcome outcome;
    {
        GuidanceLane lane(retriever);
        lane.Run(outcome.Request("busy"));
        ASSERT_TRUE(retriever.WaitUntil([&] { return retriever.searched.size() == 1; }));
        lane.Run(outcome.Request("typed two"));
        lane.Run(outcome.Request("s1 first", 3, "s1"));
        lane.Run(outcome.Request("s2 first", 3, "s2"));
        lane.Run(outcome.Request("typed three"));
        lane.Run(outcome.Request("s1 second", 3, "s1"));
        retriever.Release();
        ASSERT_TRUE(outcome.WaitUntil([&] { return outcome.ready.size() == 4; }));
    }
    EXPECT_EQ(retriever.searched,
              (std::vector<std::string>{"busy", "s1 second", "s2 first", "typed three"}));
    EXPECT_EQ(retriever.modes, (std::vector<SearchMode>{SearchMode::kQuery, SearchMode::kNote,
                                                        SearchMode::kNote, SearchMode::kQuery}));
    EXPECT_EQ(outcome.ready,
              (std::vector<std::string>{"busy", "s1 second", "s2 first", "typed three"}));
    EXPECT_NE(outcome.last_thread, std::this_thread::get_id()) << "results arrive off the caller";
    EXPECT_EQ(outcome.failed, (std::vector<std::string>{"superseded", "superseded"}));
}

TEST(GuidanceLane, PrepareRunsOnceOnTheWorkerAndReportsHowLoadingEnded) {
    std::vector<Readiness> heard;
    auto listen = [&](const Readiness& readiness) { heard.push_back(readiness); };
    {
        FakeRetriever retriever;
        retriever.hold = true;
        Outcome outcome;
        GuidanceLane lane(retriever, listen);
        lane.Run(outcome.Request("busy"));
        ASSERT_TRUE(retriever.WaitUntil([&] { return retriever.searched.size() == 1; }));
        lane.Prepare();
        lane.Prepare();
        retriever.Release();
        lane.Run(outcome.Request("after prepare"));
        ASSERT_TRUE(outcome.WaitUntil([&] { return outcome.ready.size() == 2; }));
        EXPECT_EQ(retriever.prepares, 1) << "two requests while busy load once";
    }
    {
        FakeRetriever retriever;
        retriever.prepare_throws = true;
        Outcome outcome;
        GuidanceLane lane(retriever, listen);
        lane.Prepare();
        lane.Run(outcome.Request("Chest pain on exertion."));
        ASSERT_TRUE(outcome.WaitUntil([&] { return outcome.failed.size() == 1; }));
        EXPECT_EQ(outcome.failed[0], "no embedding model staged") << "the load failure surfaces";
        EXPECT_EQ(retriever.prepares, 1);
    }
    ASSERT_EQ(heard.size(), 2u);
    EXPECT_EQ(heard[0].phase, Readiness::Phase::kReady);
    EXPECT_EQ(heard[1].phase, Readiness::Phase::kUnavailable);
    EXPECT_EQ(heard[1].detail, "no embedding model staged");
}

TEST(GuidanceLane, AFailureCallbackThatThrowsDoesNotStopTheWorker) {
    FakeRetriever retriever;
    retriever.search_throws = true;
    Outcome outcome;
    GuidanceLane lane(retriever);
    SearchRequest throwing = outcome.Request("first");
    throwing.on_failed = [](const std::string&) { throw std::runtime_error("shell gone"); };
    lane.Run(std::move(throwing));
    lane.Run(outcome.Request("second"));
    ASSERT_TRUE(outcome.WaitUntil([&] { return outcome.failed.size() == 1; }));
    EXPECT_EQ(outcome.failed[0], "corpus gone") << "the next search reports its detail";
    EXPECT_TRUE(outcome.ready.empty());
}

}  // namespace
}  // namespace clinicavt::guidance
