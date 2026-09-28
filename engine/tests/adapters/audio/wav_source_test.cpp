#include "adapters/audio/wav_source.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace clinicavt::audio {
namespace {

// Every file is built byte by byte in the test, so each malformation is
// deliberate rather than an accident of a fixture on disk.
struct WavSpec {
    std::uint16_t format = 1;  // 1 = PCM, 3 = float
    std::uint16_t channels = 1;
    std::uint32_t sample_rate = 16000;
    std::uint16_t bits_per_sample = 16;
    std::vector<std::uint8_t> data;
    std::vector<std::uint8_t> chunk_before_data;
    std::optional<std::uint32_t> declared_data_bytes;
};

template <typename T>
void AppendValue(std::vector<std::uint8_t>& out, T value) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

void AppendTag(std::vector<std::uint8_t>& out, const char* tag) {
    out.insert(out.end(), tag, tag + 4);
}

std::vector<std::uint8_t> Build(const WavSpec& spec) {
    std::vector<std::uint8_t> body;
    AppendTag(body, "WAVE");

    AppendTag(body, "fmt ");
    AppendValue<std::uint32_t>(body, 16);
    AppendValue(body, spec.format);
    AppendValue(body, spec.channels);
    AppendValue(body, spec.sample_rate);
    const std::uint32_t frame_bytes = spec.channels * spec.bits_per_sample / 8u;
    AppendValue<std::uint32_t>(body, spec.sample_rate * frame_bytes);
    AppendValue<std::uint16_t>(body, static_cast<std::uint16_t>(frame_bytes));
    AppendValue(body, spec.bits_per_sample);

    body.insert(body.end(), spec.chunk_before_data.begin(), spec.chunk_before_data.end());

    AppendTag(body, "data");
    AppendValue<std::uint32_t>(
        body, spec.declared_data_bytes.value_or(static_cast<std::uint32_t>(spec.data.size())));
    body.insert(body.end(), spec.data.begin(), spec.data.end());

    std::vector<std::uint8_t> file;
    AppendTag(file, "RIFF");
    AppendValue<std::uint32_t>(file, static_cast<std::uint32_t>(body.size()));
    file.insert(file.end(), body.begin(), body.end());
    return file;
}

struct TempWav {
    std::filesystem::path path;

    explicit TempWav(const std::vector<std::uint8_t>& bytes) {
        path =
            std::filesystem::temp_directory_path() /
            ("clinicavt-wav-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
             "-" + ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".wav");
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    ~TempWav() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
};

struct RecordingSink : IAudioSink {
    std::vector<float> frames;
    std::vector<std::size_t> packet_sizes;
    std::uint64_t lost = 0;
    std::vector<SourceEnd> ends;

    void OnAudio(std::span<const float> packet, std::uint64_t lost_frames) override {
        frames.insert(frames.end(), packet.begin(), packet.end());
        packet_sizes.push_back(packet.size());
        lost += lost_frames;
    }

    void OnEnd(const SourceEnd& end) override {
        ends.push_back(end);
    }
};

std::vector<std::uint8_t> Pcm16Bytes(const std::vector<std::int16_t>& samples) {
    std::vector<std::uint8_t> bytes;
    for (const auto sample : samples) {
        AppendValue(bytes, sample);
    }
    return bytes;
}

std::vector<std::uint8_t> FloatBytes(const std::vector<float>& samples) {
    std::vector<std::uint8_t> bytes;
    for (const auto sample : samples) {
        AppendValue(bytes, sample);
    }
    return bytes;
}

// Runs a file through a fresh source on this thread
RecordingSink Play(const std::vector<std::uint8_t>& bytes) {
    const TempWav file(bytes);
    WavSource source(file.path.string());
    RecordingSink sink;
    source.Run(sink);
    return sink;
}

TEST(WavSource, ReadsPcm16AndFloat32ExactlyInFixedPacketsPastUnknownChunks) {
    std::vector<std::uint8_t> list_chunk;
    AppendTag(list_chunk, "LIST");
    AppendValue<std::uint32_t>(list_chunk, 3);  // Odd size forces the pad byte
    list_chunk.insert(list_chunk.end(), {'a', 'b', 'c', 0});

    const std::vector<float> floats{0.25F, -0.75F, 1.0F, -1.0F};
    struct Case {
        std::string name;
        WavSpec spec;
        std::vector<float> frames;
        std::vector<std::size_t> packets;
    };
    const std::vector<Case> cases = {
        {"pcm16 with pinned scaling",
         {.data = Pcm16Bytes({-32768, -16384, 0, 16384, 32767})},
         {-1.0F, -0.5F, 0.0F, 0.5F, 32767.0F / 32768.0F},
         {5}},
        {"float32 exactly",
         {.format = 3, .bits_per_sample = 32, .data = FloatBytes(floats)},
         floats,
         {4}},
        {"fixed packets with a short tail",
         {.data = Pcm16Bytes(std::vector<std::int16_t>(1000, 7))},
         std::vector<float>(1000, 7.0F / 32768.0F),
         {480, 480, 40}},
        // LIST chunks are common in real recordings
        {"an unknown chunk and its pad byte are skipped",
         {.data = Pcm16Bytes({1, 2, 3}), .chunk_before_data = list_chunk},
         {1.0F / 32768.0F, 2.0F / 32768.0F, 3.0F / 32768.0F},
         {3}},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        const auto sink = Play(Build(c.spec));
        ASSERT_EQ(sink.ends.size(), 1u);
        EXPECT_EQ(sink.ends[0].reason, SourceEndReason::kCompleted) << sink.ends[0].detail;
        EXPECT_EQ(sink.frames, c.frames);
        EXPECT_EQ(sink.packet_sizes, c.packets);
        EXPECT_EQ(sink.lost, 0u);
    }
}

TEST(WavSource, RefusesMalformedAndUnsupportedFiles) {
    struct Case {
        std::string name;
        std::vector<std::uint8_t> bytes;
        std::string detail;
        std::size_t delivered;
    };
    const std::vector<Case> cases = {
        // A truncated file fails rather than completing, after the whole packets before the cut
        {"a truncated data chunk",
         Build(
             {.data = Pcm16Bytes(std::vector<std::int16_t>(500, 7)), .declared_data_bytes = 2000}),
         "truncated", 480},
        {"garbage", {'n', 'o', 't', ' ', 'a', ' ', 'w', 'a', 'v'}, "not a RIFF file", 0},
        {"stereo", Build({.channels = 2, .data = Pcm16Bytes({1, 2})}), "mono", 0},
        {"44.1 kHz", Build({.sample_rate = 44100, .data = Pcm16Bytes({1})}), "16000", 0},
        {"24-bit", Build({.bits_per_sample = 24, .data = {0, 0, 0}}), "PCM16 and float32", 0},
        {"3 bytes of PCM16", Build({.data = {1, 2, 3}}), "whole frames", 0},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        const auto sink = Play(c.bytes);
        ASSERT_EQ(sink.ends.size(), 1u);
        EXPECT_EQ(sink.ends[0].reason, SourceEndReason::kFailed);
        EXPECT_NE(sink.ends[0].detail.find(c.detail), std::string::npos) << sink.ends[0].detail;
        EXPECT_EQ(sink.frames.size(), c.delivered);
    }
}

TEST(WavSource, RealTimeReplayIsPacedFlatOutIsNot) {
    // Half a second of audio: paced delivery takes roughly that long
    const TempWav file(Build({.data = Pcm16Bytes(std::vector<std::int16_t>(8000, 0))}));
    RecordingSink sink;

    const auto flat_start = std::chrono::steady_clock::now();
    WavSource(file.path.string()).Run(sink);
    const auto flat = std::chrono::steady_clock::now() - flat_start;
    EXPECT_LT(flat, std::chrono::milliseconds(200));

    const auto paced_start = std::chrono::steady_clock::now();
    WavSource(file.path.string(), {.speed = 1.0}).Run(sink);
    const auto paced = std::chrono::steady_clock::now() - paced_start;
    EXPECT_GT(paced, std::chrono::milliseconds(400));
    EXPECT_LT(paced, std::chrono::milliseconds(1500));
}

// Stops the source from inside the first packet, on the source's own thread, so the next
// turn of its loop is certain to see it
struct StoppingSink : IAudioSink {
    WavSource& source;
    std::size_t frames = 0;
    std::vector<SourceEnd> ends;

    explicit StoppingSink(WavSource& source) : source(source) {}

    void OnAudio(std::span<const float> packet, std::uint64_t) override {
        frames += packet.size();
        source.RequestStop();
    }

    void OnEnd(const SourceEnd& end) override {
        ends.push_back(end);
    }
};

TEST(WavSource, StopEndsTheStreamAtTheNextPacket) {
    const TempWav file(Build({.data = Pcm16Bytes(std::vector<std::int16_t>(4800, 0))}));
    WavSource source(file.path.string());
    StoppingSink sink(source);
    source.Run(sink);
    ASSERT_EQ(sink.ends.size(), 1u);
    EXPECT_EQ(sink.ends[0].reason, SourceEndReason::kStopped) << "stopped, not completed";
    EXPECT_EQ(sink.frames, 480u) << "one packet, then the stop honoured";
}

}  // namespace
}  // namespace clinicavt::audio
