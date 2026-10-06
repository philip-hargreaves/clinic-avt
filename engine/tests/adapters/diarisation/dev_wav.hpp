#pragma once

#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace clinicavt::diar {

// Reads the research dev wav (16 kHz mono PCM16, plain 44-byte header) as float32 [-1, 1]
// It is not in the repo, so the tests that use it skip without it
inline std::vector<float> LoadDevWav(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) throw std::runtime_error("missing dev wav: " + path);
    in.seekg(0, std::ios::end);
    const auto bytes = static_cast<std::size_t>(in.tellg()) - 44;
    in.seekg(44);
    std::vector<std::int16_t> pcm(bytes / 2);
    in.read(reinterpret_cast<char*>(pcm.data()), static_cast<std::streamsize>(bytes));
    std::vector<float> frames(pcm.size());
    for (std::size_t i = 0; i < pcm.size(); ++i) frames[i] = pcm[i] / 32768.0f;
    return frames;
}

}  // namespace clinicavt::diar
