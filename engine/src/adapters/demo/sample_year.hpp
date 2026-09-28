#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/common/iso8601.hpp"
#include "core/note/summary_scrub.hpp"
#include "ports/session_store.hpp"

namespace clinicavt::demo {

// One seeded consultation from demo/reflections: the app's transcript of a real recording, the
// notes its best model wrote, and hand-written answers. Dated relative to seed time
struct Sample {
    std::string source;
    int months_back = 0;
    int day = 1;
    int hour = 9;
    int minute = 0;
    std::string label;
    std::string note;
    std::string patient;
    std::string summary;
    double audio_seconds = 0;
    std::vector<asr::Turn> turns;
    std::string happened;
    std::string learned;
    std::string next;
};

inline std::vector<Sample> LoadSampleYear(const std::filesystem::path& dir) {
    using nlohmann::json;
    std::vector<std::filesystem::path> files;
    if (std::filesystem::is_directory(dir)) {
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (entry.path().extension() == ".json") files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    std::vector<Sample> samples;
    for (const auto& file : files) {
        std::ifstream in(file);
        if (!in.is_open()) throw std::runtime_error("cannot read " + file.string());
        const json j = json::parse(in);
        Sample sample;
        sample.source = j.at("source").get<std::string>();
        sample.months_back = j.at("monthsBack").get<int>();
        sample.day = j.at("day").get<int>();
        const std::string time = j.at("time").get<std::string>();
        if (time.size() != 5 || time[2] != ':')
            throw std::runtime_error("bad time in " + file.string());
        sample.hour = std::stoi(time.substr(0, 2));
        sample.minute = std::stoi(time.substr(3, 2));
        sample.label = j.at("label").get<std::string>();
        sample.note = j.at("note").get<std::string>();
        sample.patient = j.at("patient").get<std::string>();
        sample.summary = j.at("summary").get<std::string>();
        sample.audio_seconds = j.at("audioSeconds").get<double>();
        for (const auto& t : j.at("turns")) {
            asr::Turn turn;
            turn.first_frame = t.at("firstFrame").get<std::uint64_t>();
            turn.frame_count = t.at("frameCount").get<std::uint64_t>();
            turn.speaker = t.at("speaker").get<std::string>();
            turn.text = t.at("text").get<std::string>();
            sample.turns.push_back(std::move(turn));
        }
        const auto& answers = j.at("answers");
        sample.happened = answers.at("happened").get<std::string>();
        sample.learned = answers.at("learned").get<std::string>();
        sample.next = answers.at("next").get<std::string>();
        samples.push_back(std::move(sample));
    }
    return samples;
}

// Start time: months_back months before now, the day clamped to the month
inline std::chrono::sys_seconds SampleStart(const Sample& sample, std::chrono::sys_seconds now) {
    using namespace std::chrono;
    const year_month_day today{floor<days>(now)};
    year_month ym = year_month{today.year(), today.month()} - months{sample.months_back};
    const auto month_end =
        static_cast<unsigned>(year_month_day_last{ym.year(), month_day_last{ym.month()}}.day());
    const auto day_number = std::clamp(static_cast<unsigned>(sample.day), 1u, month_end);
    const year_month_day ymd{ym.year(), ym.month(), day{day_number}};
    return sys_days{ymd} + hours{sample.hour} + minutes{sample.minute};
}

// Writes each sample as a finalised session with its documents and returns the count
inline std::size_t SeedSampleYear(store::ISessionStore& sessions,
                                  const std::vector<Sample>& samples,
                                  std::chrono::sys_seconds now) {
    using nlohmann::json;
    std::size_t written = 0;
    for (const Sample& sample : samples) {
        store::SessionSeed seed;
        const auto start = SampleStart(sample, now);
        seed.started_at = Iso8601(start);
        seed.ended_at =
            Iso8601(start + std::chrono::seconds(static_cast<long long>(sample.audio_seconds)));
        seed.turns = sample.turns;
        const auto id = sessions.Seed(seed);
        sessions.SaveDocument(id, store::DocumentKind::kLabel, {.text = sample.label});
        sessions.SaveDocument(id, store::DocumentKind::kNote,
                              {.text = sample.note, .style = "prose", .detail = "concise"});
        sessions.SaveDocument(id, store::DocumentKind::kPatient, {.text = sample.patient});
        sessions.SaveDocument(id, store::DocumentKind::kSummary,
                              {.text = note::ScrubSummary(sample.summary)});
        const json answers{
            {"happened", sample.happened}, {"learned", sample.learned}, {"next", sample.next}};
        sessions.SaveDocument(id, store::DocumentKind::kReflection, {.text = answers.dump()});
        written += 1;
    }
    return written;
}

inline bool HasSamples(store::ISessionStore& sessions) {
    for (const auto& session : sessions.ListSessions()) {
        if (session.demo) return true;
    }
    return false;
}

}  // namespace clinicavt::demo
