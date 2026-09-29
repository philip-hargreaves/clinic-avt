// Builds a guidance corpus from a build spec with the staged embedder, so index
// and query embeddings come from the same code.
//
//   clinicavt_index <spec.json> <out-dir> [--models <root>] [--verify]

#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#include "adapters/guidance/corpus_store.hpp"
#include "adapters/guidance/embedder.hpp"
#include "adapters/models/model_store.hpp"
#include "adapters/system/exe_paths.hpp"
#include "adapters/system/stderr_log.hpp"
#include "core/common/cli_args.hpp"
#include "core/common/iso8601.hpp"
#include "tools/corpus/indexer.hpp"

namespace {

constexpr const char* kBuilder = "clinicavt_index 1";

}  // namespace

int main(int argc, char** argv) {
    clinicavt::system::LogToStderr();
    std::vector<std::string> positional(argv + 1, argv + argc);
    const std::string models_flag = clinicavt::TakeFlag(positional, "--models");
    const std::filesystem::path models_root =
        models_flag.empty() ? clinicavt::system::DefaultModelsRoot() : models_flag;
    const bool verify = clinicavt::TakeSwitch(positional, "--verify");
    if (positional.size() != 2) {
        std::fprintf(stderr,
                     "usage: clinicavt_index <spec.json> <out-dir> [--models <root>] [--verify]\n");
        return 2;
    }
    try {
        const auto spec = clinicavt::guidance::ReadBuildSpec(positional[0]);
        const std::filesystem::path out_dir = positional[1];
        const clinicavt::models::ModelStore store(models_root);
        std::printf("embedder: loading from %s\n", models_root.string().c_str());
        const auto embedder = clinicavt::guidance::Embedder::Load(store);
        const auto& identity = embedder->Identity();
        std::printf("embedder: %s %s, %d dimensions\n", identity.id.c_str(),
                    identity.rev.substr(0, 12).c_str(), identity.dim);

        const auto start = std::chrono::steady_clock::now();
        std::size_t last_reported = 0;
        const auto report = clinicavt::guidance::IndexCorpus(
            spec, *embedder, out_dir, clinicavt::Iso8601Now(), kBuilder,
            [&](std::size_t done, std::size_t total) {
                if (done - last_reported >= 500 || done == total) {
                    const auto seconds =
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
                            .count();
                    std::printf("  %zu/%zu chunks, %.0f s\n", done, total, seconds);
                    std::fflush(stdout);
                    last_reported = done;
                }
            });
        std::printf("built %s: %zu chunks, %zu over %d tokens, -> %s\n", spec.corpus.id.c_str(),
                    report.chunks, report.truncated, identity.max_tokens, out_dir.string().c_str());

        if (verify) {
            std::string reason;
            const auto opened = clinicavt::guidance::CorpusStore::Open(out_dir, identity, reason);
            if (!opened) {
                std::fprintf(stderr, "verify: refused: %s\n", reason.c_str());
                return 1;
            }
            std::printf("verify: %zu chunks loaded, every guard passed\n", opened->Size());
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "clinicavt_index: %s\n", e.what());
        return 1;
    }
}
