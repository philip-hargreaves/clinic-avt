#include "adapters/models/deferred_load.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

namespace clinicavt {
namespace {

TEST(DeferredLoad, GetWaitsForTheBuildAndRethrowsItsFailure) {
    std::atomic<bool> built{false};
    models::DeferredLoad<int> deferred("test", [&built] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        built = true;
        return std::make_unique<int>(7);
    });
    EXPECT_EQ(deferred.Get(), 7);
    EXPECT_TRUE(built.load());
    EXPECT_TRUE(deferred.Loaded());

    models::DeferredLoad<int> failed(
        "test", []() -> std::unique_ptr<int> { throw std::runtime_error("no model"); });
    EXPECT_THROW(failed.Get(), std::runtime_error);
    EXPECT_FALSE(failed.Loaded());
}

}  // namespace
}  // namespace clinicavt
