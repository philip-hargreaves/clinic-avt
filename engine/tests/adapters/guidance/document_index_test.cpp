#include "adapters/guidance/document_index.hpp"

#include <gtest/gtest.h>

#include <fstream>

#include "adapters/storage/db.hpp"
#include "adapters/system/sha256.hpp"
#include "guidance_fixture.hpp"

namespace clinicavt::guidance {
namespace {

const EmbedderIdentity kEmbedder{"words", "rev-a", 4, 64};

IndexChunk Chunk(std::int64_t ord, int page, const char* text) {
    return {ord, page, "1.1", "Recommendations", text, {1.0F, 0.0F, 0.0F, 0.0F}, "[]"};
}

TEST(DocumentIndex, HoldsFilesByContentAndReleasesADocumentNoFileHolds) {
    const std::string abc = "abc";
    EXPECT_EQ(system::Sha256Hex({reinterpret_cast<const std::uint8_t*>(abc.data()), abc.size()}),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // The id is the hash's leading 63 bits, never 0
    EXPECT_EQ(DocumentIndex::IdOf(std::string(64, 'f')), 0x7FFFFFFFFFFFFFFF);
    EXPECT_EQ(DocumentIndex::IdOf(std::string(64, '0')), 1);

    fixture::TempDir dir{"index"};
    DocumentIndex index(dir.path / kIndexFile);
    EXPECT_FALSE(index.Adopted());
    index.Adopt(kEmbedder);
    EXPECT_TRUE(index.Adopted());
    EXPECT_TRUE(index.List().empty());

    const std::string one(64, 'a');
    const std::string two(64, 'b');
    auto held = index.Hold({"BSR PMR 2009.pdf", 0, 100, 7}, one, "application/pdf");
    EXPECT_TRUE(held.added);
    EXPECT_EQ(held.document, DocumentIndex::IdOf(one));
    EXPECT_FALSE(index.Hold({"copies/PMR again.pdf", 0, 100, 7}, one, "application/pdf").added)
        << "the same content at a second path is one document";
    EXPECT_EQ(index.PathsOf(held.document).size(), 2u);
    EXPECT_EQ(index.List()[0].name, "BSR PMR 2009") << "the shortest path names it";

    EXPECT_EQ(index.Release("BSR PMR 2009.pdf"), 0) << "a file still holds it";
    EXPECT_EQ(index.Release("copies/PMR again.pdf"), held.document);
    EXPECT_TRUE(index.List().empty());

    held = index.Hold({"BSR PMR 2009.pdf", 0, 100, 7}, one, "application/pdf");
    EXPECT_TRUE(held.added);
    const auto changed = index.Hold({"BSR PMR 2009.pdf", 0, 120, 9}, two, "application/pdf");
    EXPECT_TRUE(changed.added);
    EXPECT_EQ(changed.released, held.document) << "the old content has no file now";
    const auto files = index.Files();
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0].document, changed.document);
    EXPECT_EQ(files[0].modified, 9);

    index.Finish(changed.document,
                 {Chunk(0, 1, "Start prednisolone 15 mg daily."), Chunk(1, 2, "Taper slowly.")}, 5,
                 0);
    const auto rows = index.List();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].state, "ready");
    EXPECT_EQ(rows[0].path, "BSR PMR 2009.pdf");
    EXPECT_EQ(rows[0].sha256, two);
    EXPECT_EQ(rows[0].bytes, 120);
    EXPECT_EQ(rows[0].pages, 5);
    EXPECT_EQ(rows[0].chunks, 2);
    const auto chunks = index.ReadChunks(changed.document);
    ASSERT_EQ(chunks.size(), 2u);
    EXPECT_EQ(chunks[1].ord, 1);
    EXPECT_EQ(chunks[1].page, 2);
    const auto first = index.ReadChunk(changed.document, 0);
    EXPECT_EQ(first.text, "Start prednisolone 15 mg daily.");
    EXPECT_EQ(first.vector, (std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F}));

    index.Fail(changed.document, "noText", 5, 5);
    const auto failed = index.Get(changed.document);
    EXPECT_EQ(failed.state, "failed");
    EXPECT_EQ(failed.chunks, 0);
    EXPECT_THROW(index.Get(12345), store::StoreError);
    EXPECT_THROW(index.Fail(12345, "noText"), store::StoreError);
}

TEST(DocumentIndex, AnotherEmbedderEmptiesItAndAForeignFileIsMadeAgain) {
    fixture::TempDir dir{"index-embedder"};
    const auto file = dir.path / kIndexFile;
    {
        DocumentIndex index(file);
        index.Adopt(kEmbedder);
        const auto held = index.Hold({"gout.md", 0, 10, 1}, std::string(64, 'c'), "text/markdown");
        index.Finish(held.document, {Chunk(0, 0, "Offer allopurinol.")}, 0, 0);
    }
    {
        DocumentIndex index(file);
        EXPECT_FALSE(index.Adopted());
        EXPECT_EQ(index.Embedder().rev, "rev-a") << "the embedder is remembered";
        ASSERT_EQ(index.List().size(), 1u) << "listing needs no embedder";
        index.Adopt(kEmbedder);
        EXPECT_EQ(index.List().size(), 1u);
        index.Adopt({"words", "rev-b", 4, 64});
        EXPECT_TRUE(index.List().empty());
        EXPECT_TRUE(index.Files().empty());
        EXPECT_EQ(index.Embedder().rev, "rev-b");
    }
    {
        store::Db other(file, store::Db::Mode::kBuild);
        other.SetApplicationId(0x41424344);
    }
    DocumentIndex again(file);
    EXPECT_TRUE(again.List().empty());
    EXPECT_FALSE(again.Adopted()) << "a foreign file was deleted and made again";
    std::ofstream(file, std::ios::binary | std::ios::trunc) << "not a database";
    DocumentIndex once_more(file);
    EXPECT_TRUE(once_more.Files().empty());
}

}  // namespace
}  // namespace clinicavt::guidance
