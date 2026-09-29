#pragma once

#include <filesystem>
#include <utility>
#include <vector>

#include "ports/sample_source.hpp"

namespace clinicavt::demo {

// Reads the sample year from a folder of JSON files, one per consultation, in file name order
class JsonSampleSource final : public ISampleSource {
   public:
    explicit JsonSampleSource(std::filesystem::path dir) : dir_(std::move(dir)) {}

    std::vector<Sample> Load() override;

   private:
    std::filesystem::path dir_;
};

}  // namespace clinicavt::demo
