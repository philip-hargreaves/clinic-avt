#include "adapters/models/model_store.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "adapters/system/exe_paths.hpp"
#include "adapters/system/sha256.hpp"

namespace clinicavt::models {
namespace {

// Known SHA-256 digests, so fixtures need no hashing of their own
constexpr const char* kHelloHash =
    "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824";
constexpr const char* kEmptyHash =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

struct TempRoot {
    std::filesystem::path path;

    TempRoot() {
        path = std::filesystem::temp_directory_path() /
               ("clinicavt-models-" +
                std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "-" +
                ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::create_directories(path);
    }

    ~TempRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

void WriteFile(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << content;
}

std::string Files(const char* name, const char* hash) {
    return std::string(R"({")") + name + R"(": ")" + hash + R"("})";
}

// A staged model: weights.bin holding "hello" and a manifest with the
// required fields. Rows vary what they need
struct Manifest {
    std::string id = "m";
    std::string task = "asr";
    std::string tier = "default";
    std::string runtime = R"({"device": "GPU"})";
    std::string files = Files("weights.bin", kHelloHash);
    std::string extra;  // further top-level fields, each with a leading comma
};

std::filesystem::path Stage(const std::filesystem::path& root, const Manifest& m) {
    const auto dir = root / m.id;
    WriteFile(dir / "weights.bin", "hello");
    WriteFile(dir / "manifest.json", R"({"manifestVersion": 1, "id": ")" + m.id +
                                         R"(", "task": ")" + m.task + R"(", "tier": ")" + m.tier +
                                         R"(", "licence": "MIT", "runtime": )" + m.runtime +
                                         R"(, "files": )" + m.files + m.extra + "}");
    return dir;
}

// What a refused check says, empty when it passes
std::string Refusal(const std::function<void()>& check) {
    try {
        check();
    } catch (const std::runtime_error& e) {
        return e.what();
    }
    return "";
}

bool Says(const std::string& message, const std::string& part) {
    return message.find(part) != std::string::npos;
}

TEST(ModelStore, AScanListsOnlyManifestedModelsById) {
    TempRoot root;
    EXPECT_TRUE(ModelStore(root.path / "nowhere").List().empty()) << "a missing root";
    EXPECT_TRUE(ModelStore(root.path).List().empty()) << "an empty root";

    WriteFile(root.path / "half-staged" / "weights.bin", "hello");
    WriteFile(root.path / "stray.txt", "not a model");
    Stage(root.path, {.id = "whisper-turbo-int8"});
    Stage(root.path, {.id = "silero-vad", .task = "vad"});

    const ModelStore store(root.path);
    ASSERT_EQ(store.List().size(), 2u) << "a directory without a manifest is invisible";
    EXPECT_EQ(store.List()[0].id, "silero-vad");
    EXPECT_EQ(store.List()[1].id, "whisper-turbo-int8");
    EXPECT_EQ(store.List()[1].task, "asr");
    EXPECT_EQ(store.List()[1].device, "GPU");
    EXPECT_EQ(store.List()[1].licence, "MIT");
}

// Installed model folders are read-only
TEST(ModelStore, EachModelCompilesIntoItsOwnFolderUnderLocalAppData) {
    TempRoot root;
    Stage(root.path, {.id = "silero-vad", .task = "vad"});
    const ModelStore store(root.path);
    EXPECT_EQ(store.List().at(0).cache_dir, system::LocalDataRoot() / "cache" / "silero-vad");
}

// A second spelling of the root would mean a second compile cache, and a
// recompile of every model that takes minutes for the largest
TEST(ModelStore, EverySpellingOfTheRootGivesOneModelDirectory) {
    TempRoot root;
    Stage(root.path, {.id = "silero-vad", .task = "vad"});
    const auto expected = ModelStore(root.path).List().at(0).dir;

    const auto roundabout = (root.path / "silero-vad" / "..").generic_string();
    EXPECT_EQ(ModelStore(roundabout).List().at(0).dir, expected);

    const auto link = root.path.parent_path() / (root.path.filename().string() + "-link");
    const std::string mklink =
        "mklink /J \"" + link.string() + "\" \"" + root.path.string() + "\" >nul 2>&1";
    // NOLINTNEXTLINE(bugprone-command-processor) mklink is a cmd built-in
    if (std::system(mklink.c_str()) != 0) GTEST_SKIP() << "no junction";
    const auto through_link = ModelStore(link).List().at(0).dir;
    std::filesystem::remove(link);
    EXPECT_EQ(through_link, expected);
}

TEST(ModelStore, VerifyAcceptsEveryWellStagedLayout) {
    TempRoot root;
    Stage(root.path, {.id = "plain"});
    Stage(root.path,
          {.id = "uppercase-hash",
           .files = Files("weights.bin",
                          "2CF24DBA5FB0A30E26E83B2AC5B9E29E1B161E5C1FA7425E73043362938B9824")});
    const auto nested =
        Stage(root.path, {.id = "subdirectory", .files = Files("sub/config.json", kEmptyHash)});
    WriteFile(nested / "sub" / "config.json", "");
    const auto empty =
        Stage(root.path, {.id = "empty-file", .files = Files("weights.bin", kEmptyHash)});
    WriteFile(empty / "weights.bin", "");
    WriteFile(Stage(root.path, {.id = "unlisted-extra"}) / "notes.txt", "left by a human");

    const ModelStore store(root.path);
    ASSERT_EQ(store.List().size(), 5u);
    for (const auto& model : store.List()) {
        EXPECT_EQ(Refusal([&] { store.Verify(model); }), "") << model.id;
        for (const auto& [name, hash] : model.file_hashes) {
            EXPECT_EQ(system::Sha256File(model.dir / name), hash)
                << model.id << ": the hash is kept lower-case, as the tools compute it";
        }
    }
}

// Integrity is established when a model arrives. The load-time check reads
// no bytes, so it is free at any size
TEST(ModelStore, VerifyRefusesAMissingOrChangedFileByName) {
    TempRoot root;
    std::filesystem::remove(Stage(root.path, {.id = "missing"}) / "weights.bin");
    const auto sized =
        Stage(root.path, {.id = "sized", .extra = R"(, "bytes": {"weights.bin": 5})"});
    const auto changed = Stage(root.path, {.id = "changed"});
    const ModelStore store(root.path);
    ASSERT_EQ(store.List().size(), 3u);
    const ModelInfo& changed_model = store.List()[0];
    const ModelInfo& missing_model = store.List()[1];
    const ModelInfo& sized_model = store.List()[2];

    EXPECT_TRUE(Says(Refusal([&] { store.Verify(missing_model); }), "missing file weights.bin"));

    EXPECT_EQ(Refusal([&] { store.Verify(sized_model); }), "");
    WriteFile(sized / "weights.bin", "hel");
    const auto truncated = Refusal([&] { store.Verify(sized_model); });
    EXPECT_TRUE(Says(truncated, "weights.bin") && Says(truncated, "3 bytes"))
        << "a manifest that records sizes makes truncation loud at load: " << truncated;

    WriteFile(changed / "weights.bin", "jello");  // same size, different bytes
    EXPECT_EQ(Refusal([&] { store.Verify(changed_model); }), "") << "the load check reads no bytes";

    // A missing drive removes the whole folder, so the refusal names the folder
    std::filesystem::remove_all(changed);
    const auto gone = Refusal([&] { store.Verify(changed_model); });
    EXPECT_TRUE(Says(gone, "models folder cannot be read") && !Says(gone, "missing file")) << gone;
}

TEST(ModelStore, AManifestThisBuildCannotReadIsRefusedAtScan) {
    const std::string head = R"({"manifestVersion": 1, "id": "broken")";
    const std::string body = R"(, "task": "note", "tier": "default", "licence": "MIT", "files": )" +
                             Files("weights.bin", kHelloHash) + R"(, "runtime": )";
    const std::vector<std::pair<const char*, std::string>> rows = {
        {"a newer manifest version",
         R"({"manifestVersion": 2, "id": "broken")" + body + R"({"device": "GPU"}})"},
        {"malformed JSON", "{not json"},
        {"missing required fields", head + R"(, "task": "asr"})"},
        {"an unknown pipeline", head + body + R"({"device": "GPU", "pipeline": "diffusion"}})"},
        {"a nested property",
         head + body + R"({"device": "GPU", "properties": {"NESTED": {"a": 1}}}})"},
        {"array properties", head + body + R"({"device": "GPU", "properties": [1, 2]}})"},
    };
    for (const auto& [what, manifest] : rows) {
        TempRoot root;
        WriteFile(root.path / "broken" / "weights.bin", "hello");
        WriteFile(root.path / "broken" / "manifest.json", manifest);

        const auto refusal = Refusal([&] { ModelStore store(root.path); });
        EXPECT_TRUE(Says(refusal, "broken"))
            << what << " must be refused naming its directory, got: " << refusal;
    }
}

TEST(ModelStore, ResolvePicksByTaskAndTierAndOtherwiseNamesWhatIsInstalled) {
    TempRoot root;
    Stage(root.path, {.id = "whisper-turbo-int8"});
    Stage(root.path, {.id = "whisper-large-int8", .tier = "accuracy"});
    Stage(root.path, {.id = "one", .task = "vad"});
    Stage(root.path, {.id = "two", .task = "vad"});

    const ModelStore store(root.path);
    EXPECT_EQ(store.Resolve("asr", "default").id, "whisper-turbo-int8");
    EXPECT_EQ(store.Resolve("asr", "accuracy").id, "whisper-large-int8");

    const auto ambiguous = Refusal([&] { store.Resolve("vad", "default"); });
    EXPECT_TRUE(Says(ambiguous, "one") && Says(ambiguous, "two"))
        << "two models claiming one role are refused naming both: " << ambiguous;

    EXPECT_TRUE(Says(Refusal([&] { store.Resolve("note", "default"); }), "whisper-turbo-int8"))
        << "an absent role names what is installed";
}

// Missing runtime fields default to the LLM pipeline with no properties, so older manifests
// load unchanged
TEST(ModelStore, RuntimeFieldsAreReadAndDefaultToTheLlmPipeline) {
    TempRoot root;
    Stage(root.path, {.id = "qwen3.5-9b-int4", .task = "note"});
    Stage(root.path,
          {.id = "qwen3.6-35b-a3b-int4",
           .task = "note",
           .tier = "accuracy",
           .runtime = R"({"device": "GPU", "pipeline": "vlm", "properties": )"
                      R"({"ACTIVATIONS_SCALE_FACTOR": 32, "KV_CACHE_PRECISION": "u8"}})"});
    Stage(root.path, {.id = "gte-large-int8",
                      .task = "embedding",
                      .runtime = R"({"device": "CPU", "pipeline": "embedding"})"});

    const ModelStore store(root.path);
    const auto& plain = store.Resolve("note", "default");
    EXPECT_EQ(plain.pipeline, "llm");
    EXPECT_TRUE(plain.properties.is_object());
    EXPECT_TRUE(plain.properties.empty());

    const auto& vlm = store.Resolve("note", "accuracy");
    EXPECT_EQ(vlm.pipeline, "vlm");
    EXPECT_EQ(vlm.properties["ACTIVATIONS_SCALE_FACTOR"], 32);
    EXPECT_EQ(vlm.properties["KV_CACHE_PRECISION"], "u8");

    EXPECT_EQ(store.Resolve("embedding", "default").pipeline, "embedding");
}

}  // namespace
}  // namespace clinicavt::models
