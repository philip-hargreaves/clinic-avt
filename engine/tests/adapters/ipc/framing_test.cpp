#include "adapters/ipc/framing.hpp"

#include <gtest/gtest.h>

#include <string>

namespace clinicavt::ipc {
namespace {

// A pipe read can end anywhere, so the decoder must reassemble whatever split arrives
TEST(Framing, FramesReassembleFromAnySplit) {
    FrameDecoder whole;
    whole.Push(EncodeFrame("hello"));
    EXPECT_EQ(whole.Next(), "hello");
    EXPECT_EQ(whole.Next(), std::nullopt);

    FrameDecoder empty;
    empty.Push(EncodeFrame(""));
    EXPECT_EQ(empty.Next(), "");

    FrameDecoder bytewise;
    for (char c : EncodeFrame("split across pushes")) bytewise.Push(std::string_view(&c, 1));
    EXPECT_EQ(bytewise.Next(), "split across pushes");

    FrameDecoder two;
    two.Push(EncodeFrame("first") + EncodeFrame("second"));
    EXPECT_EQ(two.Next(), "first");
    EXPECT_EQ(two.Next(), "second");
    EXPECT_EQ(two.Next(), std::nullopt);

    FrameDecoder partial;
    const std::string frame = EncodeFrame("truncated");
    partial.Push(std::string_view(frame).substr(0, frame.size() - 3));
    EXPECT_EQ(partial.Next(), std::nullopt);
    EXPECT_FALSE(partial.Failed()) << "an incomplete frame is waiting, not broken";
}

// The cap bounds what a peer can make the engine allocate
TEST(Framing, TheFrameCapIsInclusiveAndPoisonsTheStream) {
    FrameDecoder at_cap;
    at_cap.Push(EncodeFrame(std::string(kMaxFrameBytes, 'x')));
    const auto out = at_cap.Next();
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->size(), kMaxFrameBytes);

    EXPECT_THROW(EncodeFrame(std::string(kMaxFrameBytes + 1, 'x')), std::length_error);

    FrameDecoder over;
    const std::uint32_t len = kMaxFrameBytes + 1;
    std::string header;
    for (int shift : {0, 8, 16, 24}) header.push_back(static_cast<char>((len >> shift) & 0xFF));
    over.Push(header);
    EXPECT_EQ(over.Next(), std::nullopt);
    EXPECT_TRUE(over.Failed());
    over.Push(EncodeFrame("after failure"));
    EXPECT_EQ(over.Next(), std::nullopt) << "a poisoned stream stays poisoned";
}

}  // namespace
}  // namespace clinicavt::ipc
