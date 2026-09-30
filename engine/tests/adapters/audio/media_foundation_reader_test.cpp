#include "adapters/audio/media_foundation_reader.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace clinicavt::audio {
namespace {

// Each fixture is one second of 44.1 kHz stereo (the wav 8 kHz) with a 440 Hz
// tone at 0.125 on the left and silence on the right; README has the commands
std::filesystem::path Fixture(const char* name) {
    return std::filesystem::path(CLINICAVT_RECORDING_FIXTURE_DIR) / name;
}

struct TempFile {
    std::filesystem::path path;

    explicit TempFile(const std::string& name) {
        path = std::filesystem::temp_directory_path() /
               ("clinicavt-recording-" +
                std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "-" + name);
    }

    ~TempFile() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
};

double MiddleRms(const std::vector<float>& audio) {
    const std::size_t from = audio.size() / 4;
    const std::size_t to = audio.size() * 3 / 4;
    double sum = 0;
    for (std::size_t i = from; i < to; ++i) sum += double(audio[i]) * audio[i];
    return std::sqrt(sum / static_cast<double>(to - from));
}

TEST(MediaFoundationReader, DecodesEveryImportFormatToMono16k) {
    MediaFoundationReader reader;
    // The downmix averages the channels, so the tone arrives at half its level
    const double expected_rms = 0.125 / 2 / std::sqrt(2.0);

    for (const char* name :
         {"tone.m4a", "tone.mp3", "tone.wma", "tone.flac", "tone.aac", "tone.wav"}) {
        SCOPED_TRACE(name);
        std::vector<double> read;
        const auto audio =
            reader.Decode(Fixture(name), [&read](double fraction) { read.push_back(fraction); });
        EXPECT_NEAR(static_cast<double>(audio.size()), 16000.0, 1600.0);
        EXPECT_NEAR(MiddleRms(audio), expected_rms, expected_rms * 0.1);
        ASSERT_FALSE(read.empty()) << "the read reports how far it has got";
        EXPECT_TRUE(std::is_sorted(read.begin(), read.end()));
        EXPECT_GT(read.back(), 0.9);
        EXPECT_LE(read.back(), 1.0);
    }
}

TEST(MediaFoundationReader, InspectReadsContainerTimeElseFileTime) {
    using namespace std::chrono;
    MediaFoundationReader reader;

    const auto m4a = reader.Inspect(Fixture("tone.m4a"));
    EXPECT_NEAR(m4a.seconds, 1.0, 0.1);
    EXPECT_EQ(m4a.recorded_at, sys_days{2026y / September / 26} + 13h + 5min);

    // An mp3 carries no date, and its file time is when recording stopped
    const TempFile copy("tone.mp3");
    std::filesystem::copy_file(Fixture("tone.mp3"), copy.path);
    const sys_seconds stopped = sys_days{2026y / March / 2} + 9h + 30min;
    std::filesystem::last_write_time(copy.path, clock_cast<file_clock>(stopped));
    const auto mp3 = reader.Inspect(copy.path);
    EXPECT_NEAR(mp3.seconds, 1.0, 0.1);
    EXPECT_EQ(mp3.recorded_at, stopped - 1s);
}

TEST(MediaFoundationReader, RefusesWhatIsNotARecording) {
    MediaFoundationReader reader;

    const TempFile text("letter.mp3");
    std::ofstream(text.path) << "Dear Dr Smith, thank you for seeing this patient.\n";
    try {
        reader.Decode(text.path, {});
        FAIL() << "a text file decoded";
    } catch (const RecordingError& e) {
        const std::string reason = e.what();
        EXPECT_NE(reason.find("not a kind of recording Windows can read"), std::string::npos)
            << reason;
        EXPECT_EQ(reason.find("letter"), std::string::npos) << reason;
    }

    const auto missing =
        std::filesystem::temp_directory_path() / "clinicavt-no-such-folder" / "consult.m4a";
    try {
        reader.Inspect(missing);
        FAIL() << "a missing file was inspected";
    } catch (const RecordingError& e) {
        EXPECT_STREQ(e.what(), "the file is no longer there");
    }
}

}  // namespace
}  // namespace clinicavt::audio
