#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "adapters/guidance/retriever.hpp"
#include "core/guidance/guidance_query.hpp"
#include "guidance_fixture.hpp"

namespace clinicavt::guidance {
namespace {

constexpr const char* kFixtureDir = CLINICAVT_GUIDANCE_FIXTURE_DIR;
constexpr int kDim = 256;

// Hashed bag of words, four letters and up, unit length: texts sharing words
// score high, so the fixture notes find their guidelines without a model
struct WordEmbedder : IEmbedder {
    EmbedderIdentity identity{"fx-words", "rev-a", kDim, 512};
    const EmbedderIdentity& Identity() const override {
        return identity;
    }
    Embedding Embed(const std::string& text) override {
        Embedding out;
        out.vector.assign(kDim, 0.f);
        std::string word;
        auto flush = [&] {
            if (word.size() >= 4) {
                std::uint32_t h = 2166136261u;
                for (unsigned char c : word) h = (h ^ c) * 16777619u;
                out.vector[h % kDim] += 1.f;
                ++out.tokens;
            }
            word.clear();
        };
        for (unsigned char c : text) {
            if (std::isalpha(c)) {
                word.push_back(static_cast<char>(std::tolower(c)));
            } else {
                flush();
            }
        }
        flush();
        double norm = 0;
        for (float v : out.vector) norm += static_cast<double>(v) * v;
        if (norm == 0) {
            out.vector[0] = 1.f;
        } else {
            for (auto& v : out.vector) v /= static_cast<float>(std::sqrt(norm));
        }
        return out;
    }
};

// Two live corpora splitting the fixture guidelines, one built under another
// weights revision, one folder that is not a corpus
struct Root {
    fixture::TempDir dir{"root"};
    Root() {
        WordEmbedder live;
        fixture::Build(dir.path / "fixture-a", "fixture-a", live,
                       fixture::Chunks(kFixtureDir, {"fx100", "fx200"}));
        fixture::Build(dir.path / "fixture-b", "fixture-b", live,
                       fixture::Chunks(kFixtureDir, {"fx300", "fx400"}));
        WordEmbedder stale;
        stale.identity.rev = "rev-b";
        fixture::Build(dir.path / "stale", "stale", stale, fixture::Chunks(kFixtureDir, {"fx100"}));
        std::filesystem::create_directories(dir.path / "notes");
    }
    std::unique_ptr<Retriever> Make(double floor = 0.2) {
        RetrieverOptions options;
        options.floor = floor;
        return std::make_unique<Retriever>([] { return std::make_unique<WordEmbedder>(); },
                                           dir.path, options);
    }
};

std::string NoteText(const char* id) {
    for (const auto& note : fixture::Notes(kFixtureDir)) {
        if (note.id == id) return note.text;
    }
    throw std::runtime_error(std::string("no fixture note ") + id);
}

TEST(Retriever, ListsEveryCorpusWithTheStaleOneUnavailableAndAMissingRootEmpty) {
    Root root;
    auto retriever = root.Make();
    EXPECT_EQ(retriever->Status().phase, Readiness::Phase::kLoading);
    retriever->Prepare();
    EXPECT_EQ(retriever->Status().phase, Readiness::Phase::kReady);
    const auto corpora = retriever->Corpora();
    ASSERT_EQ(corpora.size(), 3u);
    EXPECT_EQ(corpora[0].id, "fixture-a");
    EXPECT_EQ(corpora[0].chunks,
              static_cast<int>(fixture::Chunks(kFixtureDir, {"fx100", "fx200"}).size()));
    EXPECT_EQ(corpora[0].embedder, "fx-words");
    EXPECT_EQ(corpora[0].built_at, "2026-09-11T00:00:00Z");
    EXPECT_EQ(corpora[0].sha256.size(), 64u);
    EXPECT_TRUE(corpora[0].unavailable.empty());
    EXPECT_EQ(corpora[1].id, "fixture-b");
    EXPECT_TRUE(corpora[1].unavailable.empty());
    EXPECT_EQ(corpora[2].id, "stale");
    EXPECT_FALSE(corpora[2].unavailable.empty());
    EXPECT_EQ(corpora[2].chunks, 0);

    // A root with nothing installed loads, lists nothing and finds nothing
    Retriever none([] { return std::make_unique<WordEmbedder>(); }, root.dir.path / "missing");
    none.Prepare();
    EXPECT_TRUE(none.Corpora().empty());
    EXPECT_EQ(none.Status().phase, Readiness::Phase::kReady);
    const auto results = none.Search("Chest pain on exertion.", 3, SearchMode::kNote);
    EXPECT_TRUE(results.shown.empty());
    EXPECT_FALSE(results.abstained);
    EXPECT_EQ(results.considered, 0);
}

TEST(Retriever, AResearchCorpusIsHiddenUntilAskedForAndReloadsLive) {
    fixture::TempDir dir{"research-root"};
    WordEmbedder words;
    fixture::Build(dir.path / "licensed", "licensed", words,
                   fixture::Chunks(kFixtureDir, {"fx100", "fx200"}));
    fixture::Build(dir.path / "demo", "demo", words, fixture::Chunks(kFixtureDir, {"fx300"}), true);

    RetrieverOptions shipped;
    shipped.floor = 0.2;
    Retriever plain([] { return std::make_unique<WordEmbedder>(); }, dir.path, shipped);
    plain.Prepare();
    const auto listed = plain.Corpora();
    ASSERT_EQ(listed.size(), 1u) << "the research corpus is not loaded or listed";
    EXPECT_EQ(listed[0].id, "licensed");
    EXPECT_FALSE(listed[0].research);
    const auto searched = plain.Search(NoteText("joint-referral"), 3, SearchMode::kNote).searched;
    for (const auto& c : searched) EXPECT_NE(c.id, "demo") << "the research corpus is not searched";

    plain.SetResearch(true);
    const auto both = plain.Corpora();
    ASSERT_EQ(both.size(), 2u) << "the research corpus appears with no restart";
    const auto demo =
        std::find_if(both.begin(), both.end(), [](const Corpus& c) { return c.id == "demo"; });
    ASSERT_NE(demo, both.end());
    EXPECT_TRUE(demo->research);
    plain.SetResearch(false);
    EXPECT_EQ(plain.Corpora().size(), 1u) << "and goes again";
}

TEST(Retriever, ANoteFindsItsGuidelineAcrossEveryCorpusWithinTheLimit) {
    Root root;
    auto retriever = root.Make();
    const auto note = NoteText("joint-referral");
    EXPECT_EQ(retriever->Status().phase, Readiness::Phase::kLoading);
    const auto results = retriever->Search(note, 3, SearchMode::kNote);
    EXPECT_EQ(retriever->Status().phase, Readiness::Phase::kReady);
    ASSERT_FALSE(results.shown.empty());
    EXPECT_LE(results.shown.size(), 3u);
    EXPECT_GE(results.considered, static_cast<int>(results.shown.size()));
    EXPECT_FALSE(results.abstained);
    EXPECT_EQ(results.shown[0].code, "fx100");
    EXPECT_EQ(results.shown[0].citation,
              "FX100 " + results.shown[0].number +
                  ", Fictional inflammatory joint disease: assessment and management");
    EXPECT_FALSE(results.shown[0].trigger.empty()) << "a sentence found it, not the whole note";
    EXPECT_EQ(results.floor, 0.2);
    EXPECT_EQ(results.searched.size(), 2u);  // the two loaded corpora, without the stale one
    const auto sentences = SplitSentences(note);
    for (const auto& r : results.shown) {
        EXPECT_EQ(r.corpus, "fixture-a");
        EXPECT_FALSE(r.chunk_id.empty());
        EXPECT_FALSE(r.title.empty());
        EXPECT_FALSE(r.section.empty());
        EXPECT_FALSE(r.text.empty());
        EXPECT_FALSE(r.number.empty());
        EXPECT_EQ(r.url, "https://example.test/" + r.chunk_id);
        EXPECT_EQ(r.source, "text");
        EXPECT_EQ(r.citation, Citation(r.code, r.number, r.title));
        EXPECT_GE(r.score, 0.2);
        if (!r.trigger.empty()) {
            EXPECT_NE(std::find(sentences.begin(), sentences.end(), r.trigger), sentences.end())
                << "a trigger is a sentence of the note";
        }
    }

    // A note on two topics draws from both corpora
    const auto both = retriever->Search(
        "Synovitis of the small joints of both hands with morning stiffness. "
        "Also reports frequent migraine with aura and asks about a triptan.",
        6, SearchMode::kNote);
    bool from_a = false, from_b = false;
    for (const auto& r : both.shown) {
        from_a |= r.corpus == "fixture-a";
        from_b |= r.corpus == "fixture-b";
    }
    EXPECT_TRUE(from_a);
    EXPECT_TRUE(from_b);

    EXPECT_EQ(retriever->Search(note, 1, SearchMode::kNote).shown.size(), 1u);
    EXPECT_TRUE(retriever->Search(note, 0, SearchMode::kNote).shown.empty());
}

TEST(Retriever, AbstainsWhenTheFloorOrThePopulationGuardLeavesNothing) {
    {
        Root root;
        const auto floored =
            root.Make(0.999)->Search(NoteText("joint-referral"), 3, SearchMode::kNote);
        EXPECT_TRUE(floored.abstained);
        EXPECT_TRUE(floored.shown.empty());
        EXPECT_GT(floored.considered, 0);
    }

    // A match for another population is dropped, leaving nothing
    fixture::TempDir dir("guarded");
    WordEmbedder live;
    Chunk only;
    only.id = "px1-1_1_1";
    only.code = "px1";
    only.title = "Fictional paediatric fever";
    only.number = "1.1.1";
    only.text = "Offer children with fever paracetamol.";
    fixture::Build(dir.path / "px1", "px1", live, {only});
    RetrieverOptions options;
    options.floor = 0.2;
    Retriever retriever([] { return std::make_unique<WordEmbedder>(); }, dir.path, options);

    EXPECT_EQ(retriever.Search("Fever offered paracetamol.", 3, SearchMode::kNote).shown.size(),
              1u);
    const auto guarded =
        retriever.Search("Adult man aged 45 with fever offered paracetamol.", 3, SearchMode::kNote);
    EXPECT_EQ(guarded.considered, 1);
    EXPECT_TRUE(guarded.shown.empty());
    EXPECT_TRUE(guarded.abstained) << "the population guard emptied the list";
}

TEST(Retriever, ATypedQueryIsSearchedWholeAndANoteSentenceBySentence) {
    Root root;
    auto retriever = root.Make();
    const auto as_note = retriever->Search("persistent synovitis", 3, SearchMode::kNote);
    EXPECT_TRUE(as_note.abstained);
    EXPECT_EQ(as_note.considered, 0);
    const auto as_query = retriever->Search("persistent synovitis", 3, SearchMode::kQuery);
    EXPECT_GT(as_query.considered, 0);
    ASSERT_FALSE(as_query.shown.empty());
    EXPECT_EQ(as_query.shown[0].code, "fx100");
    EXPECT_TRUE(as_query.shown[0].trigger.empty());

    const auto two_sentences = retriever->Search(
        "Synovitis of the small joints of both hands. Migraine with aura and asks about a triptan.",
        6, SearchMode::kQuery);
    ASSERT_FALSE(two_sentences.shown.empty());
    for (const auto& r : two_sentences.shown) EXPECT_TRUE(r.trigger.empty()) << "split as a note";
    EXPECT_TRUE(retriever->Search("  ", 3, SearchMode::kQuery).abstained);

    // Blank text abstains without embedding anything
    const auto blank = retriever->Search("  ", 3, SearchMode::kNote);
    EXPECT_TRUE(blank.abstained);
    EXPECT_EQ(blank.considered, 0);

    // With every sentence filtered out, the whole note is the trigger
    const auto filtered = retriever->Search(
        "No persistent synovitis of the small joints of the hands. "
        "Denies any urgent referral to a specialist.",
        3, SearchMode::kNote);
    ASSERT_FALSE(filtered.shown.empty());
    for (const auto& r : filtered.shown) EXPECT_TRUE(r.trigger.empty());
}

TEST(Retriever, AMissingEmbedderIsLoadedOnceAndReportedUnavailable) {
    int loads = 0;
    Retriever retriever(
        [&]() -> std::unique_ptr<IEmbedder> {
            ++loads;
            throw std::runtime_error("no embedding model staged");
        },
        std::filesystem::temp_directory_path());
    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            retriever.Search("Chest pain on exertion.", 3, SearchMode::kNote);
            FAIL() << "search succeeded without an embedder";
        } catch (const std::runtime_error& e) {
            EXPECT_STREQ(e.what(), "no embedding model staged");
        }
    }
    EXPECT_EQ(loads, 1);
    EXPECT_TRUE(retriever.Corpora().empty());
    EXPECT_EQ(retriever.Status().phase, Readiness::Phase::kUnavailable);
    EXPECT_EQ(retriever.Status().detail, "no embedding model staged");
}

}  // namespace
}  // namespace clinicavt::guidance
