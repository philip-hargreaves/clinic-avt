#include "core/common/cli_args.hpp"

#include <gtest/gtest.h>

namespace clinicavt {
namespace {

TEST(CliArgs, TakeFlagConsumesAFlagAndItsValueOnly) {
    std::vector<std::string> args{"pipe", "--asr-device", "NPU", "store"};
    EXPECT_EQ(TakeFlag(args, "--asr-device"), "NPU");
    EXPECT_EQ(args, (std::vector<std::string>{"pipe", "store"}));

    EXPECT_EQ(TakeFlag(args, "--asr-device"), "") << "absent";
    EXPECT_EQ(args.size(), 2u);

    args.push_back("--asr-device");
    EXPECT_EQ(TakeFlag(args, "--asr-device"), "") << "a trailing flag without a value";
    EXPECT_EQ(args.size(), 3u);
}

}  // namespace
}  // namespace clinicavt
