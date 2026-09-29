#include "adapters/guidance/document_ingest.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>

#include "guidance_fixture.hpp"
#include "tiny_pdf.hpp"

namespace clinicavt::guidance {
namespace {

// Sixty-four buckets of hashed words, unit length: shared words score higher
struct WordEmbedder : IEmbedder {
    EmbedderIdentity identity{"words", "rev-1", 64, 64};
    const EmbedderIdentity& Identity() const override {
        return identity;
    }
    Embedding Embed(const std::string& text) override {
        Embedding out;
        out.vector.assign(64, 0.0F);
        unsigned hash = 0;
        bool in_word = false;
        const auto close = [&] {
            if (in_word) out.vector[hash % 64] += 1.0F;
            hash = 0;
            in_word = false;
        };
        for (const unsigned char c : text) {
            if (std::isalnum(c)) {
                hash = hash * 31 + static_cast<unsigned>(std::tolower(c));
                in_word = true;
            } else {
                close();
            }
        }
        close();
        float norm = 0;
        for (const float v : out.vector) norm += v * v;
        norm = norm > 0 ? std::sqrt(norm) : 1.0F;
        for (auto& v : out.vector) v /= norm;
        return out;
    }
};

constexpr auto kScan = std::chrono::milliseconds(100);

Retriever MakeRetriever(const std::filesystem::path& dir) {
    std::filesystem::create_directories(dir / "corpora");
    return Retriever{[] { return std::make_unique<WordEmbedder>(); }, dir / "corpora",
                     RetrieverOptions{.floor = 0.2, .upload_floor = 0.2, .upload_note_floor = 0.2}};
}

void Delete(const std::filesystem::path& path) {
    std::filesystem::remove(path);
}

// DocumentIngest over root. The folder defaults to root/guidelines
struct Harness {
    std::filesystem::path root;
    Retriever retriever;
    std::filesystem::path folder;
    std::atomic<bool> busy{false};
    std::vector<std::filesystem::path> binned;  // what Remove sent to the bin
    std::mutex mutex;
    std::vector<DocumentInfo> documents;
    std::vector<IngestProgress> progress;
    DocumentIngest ingest;  // last, so its threads stop before the members they write to

    explicit Harness(std::filesystem::path dir, std::filesystem::path host = {},
                     std::filesystem::path in = {})
        : root(std::move(dir)),
          retriever(MakeRetriever(root)),
          folder(in.empty() ? root / "guidelines" : std::move(in)),
          ingest(
              retriever, folder, root / "index", [this] { return busy.load(); }, std::move(host),
              HostLimits{},
              [this](const std::filesystem::path& path) {
                  binned.push_back(path);
                  Delete(path);
              },
              kScan) {
        retriever.Prepare();
        ingest.SetListener(
            [this](const IngestProgress& p) {
                std::lock_guard<std::mutex> lock(mutex);
                progress.push_back(p);
            },
            [this](const DocumentInfo& d) {
                std::lock_guard<std::mutex> lock(mutex);
                documents.push_back(d);
            });
    }

    // A file in the folder, or under a subfolder of it
    std::filesystem::path Write(const char* name, const std::string& text) {
        const auto path = folder / name;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
        return path;
    }

    std::filesystem::path WriteOutside(const char* name, const std::string& text) {
        const auto path = root / name;
        std::ofstream(path, std::ios::binary) << text;
        return path;
    }

    std::filesystem::path WritePdf(const char* name, const std::vector<std::string>& lines) {
        const auto pdf = fixture::TinyPdf(lines);
        return Write(name, std::string(pdf.begin(), pdf.end()));
    }

    bool WaitUntil(const std::function<bool()>& condition, int seconds = 10) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (std::chrono::steady_clock::now() < deadline) {
            if (condition()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    // Waits for the scan to list the file and returns its id
    std::int64_t IdOf(const char* path, int seconds = 10) {
        std::int64_t id = 0;
        WaitUntil(
            [&] {
                for (const auto& d : ingest.List().documents) {
                    if (d.path == path) {
                        id = d.id;
                        return true;
                    }
                }
                return false;
            },
            seconds);
        return id;
    }

    bool WaitForState(std::int64_t id, DocumentState state, int seconds = 10) {
        return WaitUntil(
            [&] {
                std::lock_guard<std::mutex> lock(mutex);
                return std::ranges::any_of(documents, [&](const DocumentInfo& d) {
                    return d.id == id && d.state == state;
                });
            },
            seconds);
    }

    // For a document seen before, whose earlier events would satisfy WaitForState
    bool WaitListed(std::size_t count, DocumentState state) {
        return WaitUntil([&] {
            const auto rows = ingest.List().documents;
            return rows.size() == count && std::ranges::all_of(rows, [&](const DocumentInfo& d) {
                       return d.state == state;
                   });
        });
    }
};

const char* const kGuideline =
    "Gout guideline\n\n1.1 Offer allopurinol after a first attack when urate stays high.\n\n"
    "1.2 Offer colchicine or an NSAID for an acute flare.\n\n"
    "1.3 Check urate six weeks after any dose change.\n";

const char* const kPathway =
    "PMR pathway\n\n1.1 Start prednisolone 15 mg daily and review the response at a week.\n\n"
    "1.2 Taper by 2.5 mg every two to four weeks once symptoms settle.\n";

bool Shows(const Results& results, std::int64_t document) {
    return std::ranges::any_of(results.shown,
                               [&](const Result& r) { return r.document == document; });
}

TEST(DocumentIngest, AFileIsIndexedSearchedCitedAndRemovedEveryWay) {
    fixture::TempDir dir{"ingest"};
    Harness h(dir.path);
    const auto gout_file = h.Write("Gout local guideline.md", kGuideline);
    const auto pmr_file = h.Write("pmr.md", kPathway);
    const auto gout = h.IdOf("Gout local guideline.md");
    const auto pmr = h.IdOf("pmr.md");
    ASSERT_NE(gout, 0);
    ASSERT_NE(pmr, 0);
    ASSERT_TRUE(h.WaitForState(gout, DocumentState::kReady));
    ASSERT_TRUE(h.WaitForState(pmr, DocumentState::kReady));

    const auto listing = h.ingest.List();
    EXPECT_EQ(listing.folder, h.folder);
    EXPECT_TRUE(listing.found);
    EXPECT_EQ(listing.unsupported, 0);
    ASSERT_EQ(listing.documents.size(), 2u);
    const auto found = std::ranges::find_if(listing.documents,
                                            [&](const DocumentInfo& d) { return d.id == gout; });
    ASSERT_NE(found, listing.documents.end());
    const auto& row = *found;
    EXPECT_EQ(row.name, "Gout local guideline");
    EXPECT_EQ(row.sha256.size(), 64u);
    EXPECT_EQ(row.id, DocumentIndex::IdOf(row.sha256));
    EXPECT_EQ(row.bytes, static_cast<std::int64_t>(std::filesystem::file_size(gout_file)));
    EXPECT_GE(row.chunks, 1);
    EXPECT_TRUE(std::filesystem::exists(h.folder / kReadMe)) << "the folder explains itself";
    {
        std::lock_guard<std::mutex> lock(h.mutex);
        EXPECT_FALSE(h.progress.empty());
        EXPECT_EQ(h.progress.back().phase, "preparing");
        EXPECT_EQ(h.progress.back().done, h.progress.back().total);
    }

    const char* const flare = "Colchicine for an acute flare of gout.";
    const auto results = h.retriever.Search(flare, 3, SearchMode::kQuery);
    ASSERT_FALSE(results.shown.empty());
    EXPECT_EQ(results.shown[0].source, "upload");
    EXPECT_EQ(results.shown[0].corpus, "upload:" + std::to_string(gout));
    EXPECT_EQ(results.shown[0].title, "Gout local guideline");
    EXPECT_EQ(results.shown[0].document, gout);
    EXPECT_NE(results.shown[0].text.find("colchicine"), std::string::npos);
    ASSERT_EQ(results.searched.size(), 2u);
    const auto searched = std::ranges::find_if(results.searched, [&](const Corpus& c) {
        return c.id == "upload:" + std::to_string(gout);
    });
    ASSERT_NE(searched, results.searched.end());
    EXPECT_EQ(searched->sha256, row.sha256);

    // A result's chunk id names the chunk within its own document
    const auto taper =
        h.retriever.Search("Taper prednisolone once symptoms settle.", 3, SearchMode::kQuery);
    ASSERT_FALSE(taper.shown.empty());
    const auto& hit = taper.shown[0];
    ASSERT_EQ(hit.document, pmr);
    const auto ord = std::stoll(hit.chunk_id.substr(hit.chunk_id.rfind('-') + 1));
    EXPECT_EQ(DocumentIndex(h.root / "index" / kIndexFile).ReadChunk(pmr, ord).text, hit.text);

    Delete(gout_file);
    ASSERT_TRUE(h.WaitForState(gout, DocumentState::kRemoved));
    EXPECT_EQ(h.ingest.List().documents.size(), 1u);
    EXPECT_FALSE(Shows(h.retriever.Search(flare, 3, SearchMode::kQuery), gout));

    h.ingest.Remove(pmr);
    EXPECT_EQ(h.binned, std::vector<std::filesystem::path>{pmr_file});
    EXPECT_FALSE(std::filesystem::exists(pmr_file));
    ASSERT_TRUE(h.WaitForState(pmr, DocumentState::kRemoved));
    EXPECT_THROW(h.ingest.Remove(pmr), store::StoreError);

    // Restoring the file restores the document. RemoveAll bins every file
    h.Write("Gout local guideline.md", kGuideline);
    ASSERT_TRUE(h.WaitListed(1, DocumentState::kReady));
    EXPECT_EQ(h.ingest.RemoveAll(), 1u);
    EXPECT_EQ(h.binned.size(), 2u);
    EXPECT_FALSE(std::filesystem::exists(gout_file));
    ASSERT_TRUE(h.WaitListed(0, DocumentState::kReady));
    EXPECT_TRUE(std::filesystem::exists(h.folder / kReadMe));
}

TEST(DocumentIngest, ARenameKeepsTheDocumentAndAChangedFileIsANewOne) {
    fixture::TempDir dir{"ingest"};
    Harness h(dir.path);
    const auto file = h.Write("pmr.md", kPathway);
    const auto id = h.IdOf("pmr.md");
    ASSERT_TRUE(h.WaitForState(id, DocumentState::kReady));
    const auto sha = h.ingest.List().documents[0].sha256;

    std::filesystem::rename(file, h.folder / "PMR pathway 2024.md");
    ASSERT_TRUE(h.WaitUntil([&] {
        const auto rows = h.ingest.List().documents;
        return rows.size() == 1 && rows[0].path == "PMR pathway 2024.md" && rows[0].id == id;
    }));
    const auto renamed = h.ingest.List().documents[0];
    EXPECT_EQ(renamed.name, "PMR pathway 2024");
    EXPECT_EQ(renamed.state, DocumentState::kReady) << "a rename does not re-index";

    h.Write("PMR pathway 2024.md", std::string(kPathway) + "\n1.3 Review bone protection.\n");
    ASSERT_TRUE(h.WaitForState(id, DocumentState::kRemoved)) << "the old content is gone";
    ASSERT_TRUE(h.WaitUntil([&] {
        const auto rows = h.ingest.List().documents;
        return rows.size() == 1 && rows[0].state == DocumentState::kReady && rows[0].sha256 != sha;
    })) << "the changed file is a new document";
    EXPECT_NE(h.ingest.List().documents[0].id, id) << "so old citations never point into new text";
}

TEST(DocumentIngest, TheShippedGuidelinesStartANewFolderAndNeverRefillOne) {
    fixture::TempDir dir{"seed"};
    const auto shipped = dir.path / "shipped";
    std::filesystem::create_directories(shipped);
    std::ofstream(shipped / "BSR gout.pdf") << "a";
    std::ofstream(shipped / "BSR lupus.pdf") << "b";
    const auto folder = dir.path / "Documents" / "ClinicAVT guidelines";

    EXPECT_EQ(SeedGuidelines(folder, shipped), 2u);
    EXPECT_TRUE(std::filesystem::exists(folder / "BSR lupus.pdf"));

    std::filesystem::remove(folder / "BSR gout.pdf");
    EXPECT_EQ(SeedGuidelines(folder, shipped), 0u) << "the clinician removed it";
    EXPECT_FALSE(std::filesystem::exists(folder / "BSR gout.pdf"));

    const auto empty = dir.path / "empty";
    std::filesystem::create_directories(empty);
    EXPECT_EQ(SeedGuidelines(empty, shipped), 0u) << "an existing folder is the clinician's";
    EXPECT_EQ(SeedGuidelines(dir.path / "other", dir.path / "nothing shipped"), 0u);
    EXPECT_FALSE(std::filesystem::exists(dir.path / "other"));
}

TEST(DocumentIngest, ADeletedFolderIsMadeAgainEmptyAndAnUnreachableParentHoldsTheRows) {
    fixture::TempDir dir{"ingest"};
    Harness h(dir.path);
    h.Write("gout.md", kGuideline);
    const auto id = h.IdOf("gout.md");
    ASSERT_TRUE(h.WaitForState(id, DocumentState::kReady));
    std::filesystem::remove_all(h.folder);
    ASSERT_TRUE(h.WaitForState(id, DocumentState::kRemoved));
    ASSERT_TRUE(h.WaitUntil([&] { return std::filesystem::exists(h.folder / kReadMe); }));
    EXPECT_TRUE(h.ingest.List().found);

    // A folder whose parent is gone, as on an unplugged drive, is out of reach
    fixture::TempDir away_dir{"ingest-unreachable"};
    const auto drive = away_dir.path / "drive";
    Harness away(away_dir.path, {}, drive / "guidelines");
    away.Write("gout.md", kGuideline);
    ASSERT_TRUE(away.WaitForState(away.IdOf("gout.md"), DocumentState::kReady));
    std::filesystem::remove_all(drive);
    // The scan that finds the folder unreachable stops before acting on anything
    ASSERT_TRUE(away.WaitUntil([&] { return !away.ingest.List().found; }));
    EXPECT_EQ(away.ingest.List().documents.size(), 1u)
        << "nothing removed while the folder is out of reach";
}

TEST(DocumentIngest, PatientDataIsRefusedAndWhatCannotBeReadIsSkippedOrCounted) {
    fixture::TempDir dir{"ingest"};
    Harness h(dir.path);
    h.Write("letter.txt",
            "Dear Dr Jones, this man's NHS number is 943 476 5919 and his DOB: 1961.");
    h.Write("notes.docx", "PK not a text file");
    h.Write("scan.pdf", "%PDF");  // no host in this harness
    h.Write("~$lock.txt", "an Office lock");
    h.Write("gout.md", kGuideline);
    const auto letter = h.IdOf("letter.txt");
    ASSERT_TRUE(h.WaitForState(letter, DocumentState::kFailed));
    ASSERT_TRUE(h.WaitForState(h.IdOf("gout.md"), DocumentState::kReady));
    const auto listing = h.ingest.List();
    EXPECT_EQ(listing.unsupported, 2) << "the docx and the pdf; the lock file is ignored";
    ASSERT_EQ(listing.documents.size(), 2u);
    for (const auto& row : listing.documents) {
        if (row.id == letter) EXPECT_EQ(row.error, "patientData");
    }

    // Add copies into the folder and skips what it cannot take
    const auto accepted =
        h.ingest.Add({h.WriteOutside("PMR pathway.md", kPathway), h.WriteOutside("empty.txt", ""),
                      h.WriteOutside("scan.pdf", "%PDF")});
    ASSERT_EQ(accepted.skipped.size(), 2u);
    EXPECT_EQ(accepted.skipped[0].reason, "unreadable");
    EXPECT_EQ(accepted.skipped[1].reason, "unsupported");
    EXPECT_TRUE(std::filesystem::exists(h.folder / "PMR pathway.md"));
    // Add copies then scans. A scan under load may take the file on the next pass
    const auto pmr = h.IdOf("PMR pathway.md");
    ASSERT_NE(pmr, 0);
    ASSERT_TRUE(h.WaitForState(pmr, DocumentState::kReady));
    EXPECT_EQ(h.ingest.Add({h.root / "PMR pathway.md"}).documents.size(), 1u)
        << "the same content again is the same document";
    EXPECT_EQ(h.ingest.List().documents.size(), 3u);
}

TEST(DocumentIngest, ReadsAPdfThroughTheHostWithItsPages) {
    fixture::TempDir dir{"ingest"};
    Harness h(dir.path, CLINICAVT_INGEST_HOST);
    const std::vector<std::string> lines = {
        "Offer allopurinol after a first attack when urate stays high.",
        "Check urate six weeks after any dose change."};
    const auto path = h.WritePdf("gout.pdf", lines);
    const auto id = h.IdOf("gout.pdf");
    ASSERT_TRUE(h.WaitForState(id, DocumentState::kReady));
    const auto row = h.ingest.List().documents[0];
    EXPECT_EQ(row.pages, 1);
    EXPECT_EQ(row.pages_without_text, 0);
    EXPECT_GE(row.chunks, 1);

    const auto results =
        h.retriever.Search("allopurinol after a first attack", 3, SearchMode::kQuery);
    ASSERT_FALSE(results.shown.empty());
    EXPECT_EQ(results.shown[0].document, id);
    EXPECT_EQ(results.shown[0].page, 0);
    EXPECT_EQ(results.shown[0].pages, 1);
    EXPECT_TRUE(results.shown[0].citation.starts_with("gout, page 1 (added "))
        << results.shown[0].citation;

    // The page view draws a bitmap with the chunk boxes under scratch. Path gives the file
    const auto drawn = h.ingest.Render(id, 0, 0);
    EXPECT_EQ(drawn.width, 1190);
    EXPECT_EQ(drawn.height, 1684);
    EXPECT_EQ(drawn.pages, 1);
    EXPECT_NE(drawn.boxes.find("\"page\":0"), std::string::npos);
    EXPECT_TRUE(std::filesystem::exists(drawn.path));
    EXPECT_EQ(drawn.path.parent_path().filename(), "scratch");
    EXPECT_EQ(h.ingest.Path(id), path);
    EXPECT_THROW(h.ingest.Render(id, 3, 0), store::StoreError);
}

TEST(DocumentIngest, HostRefusalsBecomeTheRowsError) {
    fixture::TempDir dir{"ingest"};
    Harness h(dir.path, CLINICAVT_FAKE_INGEST_HOST);
    h.Write("locked.pdf", "FAKE exit 3");
    h.Write("broken.pdf", "FAKE crash");
    h.Write("fine.pdf", "%PDF canned");
    const auto locked = h.IdOf("locked.pdf");
    const auto broken = h.IdOf("broken.pdf");
    const auto fine = h.IdOf("fine.pdf");
    ASSERT_TRUE(h.WaitForState(locked, DocumentState::kFailed));
    ASSERT_TRUE(h.WaitForState(broken, DocumentState::kFailed));
    ASSERT_TRUE(h.WaitForState(fine, DocumentState::kReady));
    for (const auto& row : h.ingest.List().documents) {
        if (row.id == locked) EXPECT_EQ(row.error, "password");
        if (row.id == broken) EXPECT_EQ(row.error, "crashed");
        if (row.id == fine) {
            EXPECT_EQ(row.pages, 2);
            EXPECT_EQ(row.pages_without_text, 1);
            EXPECT_GE(row.chunks, 1);
        }
    }
    const auto results = h.retriever.Search("Offer allopurinol", 3, SearchMode::kQuery);
    ASSERT_FALSE(results.shown.empty());
    EXPECT_EQ(results.shown[0].number, "1.1");
    EXPECT_TRUE(results.shown[0].citation.starts_with("fine, page 1, 1.1 (added "))
        << results.shown[0].citation;
}

TEST(DocumentIngest, AReadyDocumentIsListedAndSearchedAgainAfterARestart) {
    fixture::TempDir dir{"ingest"};
    {
        Harness first(dir.path);
        first.Write("guideline.md", kGuideline);
        ASSERT_TRUE(first.WaitForState(first.IdOf("guideline.md"), DocumentState::kReady));
    }

    Harness h(dir.path);
    ASSERT_EQ(h.ingest.List().documents.size(), 1u) << "listed before any scan or embedder";
    EXPECT_EQ(h.ingest.List().documents[0].state, DocumentState::kReady);
    Results results;
    ASSERT_TRUE(h.WaitUntil([&] {
        results =
            h.retriever.Search("Colchicine for an acute flare of gout.", 3, SearchMode::kQuery);
        return !results.shown.empty();
    })) << "published once the embedder was adopted";
    EXPECT_EQ(results.shown[0].source, "upload");
}

TEST(DocumentIngest, IndexingPausesWhileAConsultationRunsAndRemoveDuringItCancels) {
    fixture::TempDir dir{"ingest"};
    Harness h(dir.path);
    h.busy = true;
    const auto file = h.Write("guideline.md", kGuideline);
    // The file is found and its row appears, but embedding waits for the consultation
    const auto id = h.IdOf("guideline.md");
    ASSERT_NE(id, 0);
    bool paused = false;
    ASSERT_TRUE(h.WaitUntil([&] {
        std::lock_guard<std::mutex> lock(h.mutex);
        for (const auto& p : h.progress) paused = paused || p.phase == "paused";
        return paused;
    }));
    EXPECT_EQ(h.ingest.List().documents[0].state, DocumentState::kIndexing);

    h.ingest.Remove(id);
    EXPECT_FALSE(std::filesystem::exists(file));
    h.busy = false;
    ASSERT_TRUE(h.WaitForState(id, DocumentState::kRemoved));
    EXPECT_TRUE(h.ingest.List().documents.empty());
}

}  // namespace
}  // namespace clinicavt::guidance
