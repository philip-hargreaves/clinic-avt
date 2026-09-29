#include "adapters/demo/json_sample_source.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <utility>

namespace clinicavt::demo {

std::vector<Sample> JsonSampleSource::Load() {
    using nlohmann::json;
    std::vector<std::filesystem::path> files;
    if (std::filesystem::is_directory(dir_)) {
        for (const auto& entry : std::filesystem::directory_iterator(dir_)) {
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

}  // namespace clinicavt::demo
