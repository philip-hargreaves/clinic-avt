#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "adapters/guidance/embedder.hpp"
#include "tools/corpus/chunker.hpp"
#include "tools/corpus/corpus_builder.hpp"

namespace clinicavt::guidance {

// What to index and how to describe it, read from a build spec json
struct BuildSpec {
    CorpusSpec corpus;
    std::filesystem::path nice_dir;  // source "nice": the extracted corpus, json/ and manifest.json
    std::filesystem::path codes;     // source "nice": the requested guideline codes, one per line
    std::filesystem::path text_dir;  // source "text": a folder of .md and .txt files
};

BuildSpec ReadBuildSpec(const std::filesystem::path& json);

// Guideline codes, lower case, comments after # dropped
std::set<std::string> ReadCodes(const std::filesystem::path& file);

// Every requested, current, non-stub guideline under dir/json, one chunk per
// recommendation, duplicate renderings skipped
std::vector<Chunk> ChunksFromNiceDir(const std::filesystem::path& dir,
                                     const std::set<std::string>& requested);

// Every .md and .txt file in dir: code is the file stem, title its first
// heading or the stem
std::vector<Chunk> ChunksFromTextDir(const std::filesystem::path& dir);

struct IndexReport {
    std::size_t chunks = 0;
    std::size_t truncated = 0;  // chunks longer than the embedder's max tokens
};

using Progress = std::function<void(std::size_t done, std::size_t total)>;

// Chunks the source, embeds every chunk, writes the corpus into out_dir
IndexReport IndexCorpus(const BuildSpec& spec, IEmbedder& embedder,
                        const std::filesystem::path& out_dir, const std::string& built_at,
                        const std::string& builder, const Progress& progress = {});

}  // namespace clinicavt::guidance
