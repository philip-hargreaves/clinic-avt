#include "adapters/guidance/folder_scan.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include "guidance_fixture.hpp"

namespace clinicavt::guidance {
namespace {

TEST(FolderScan, ListsSupportedFilesToTheDepthAndCountsTheRest) {
    fixture::TempDir dir{"folder-scan"};
    const auto put = [&](const std::string& relative, const std::string& text = "x") {
        std::filesystem::create_directories((dir.path / relative).parent_path());
        std::ofstream(dir.path / relative, std::ios::binary) << text;
    };
    put("one.PDF", "pdf");  // the extension is matched in any case
    put("Instructions.txt");
    put("~lock.pdf");
    put("notes.docx");
    put("a/two.txt");
    put("a/b/three.md");
    put("a/b/c/four.md");
    put("a/b/c/d/five.md");  // one deeper than the limit

    const auto supported = [](const std::string& mime) { return !mime.empty(); };
    const auto listing = ListFolder(dir.path, 3, supported, "Instructions.txt");

    std::set<std::string> paths;
    for (const auto& file : listing.files) paths.insert(file.path);
    EXPECT_EQ(paths, (std::set<std::string>{"one.PDF", "a\\two.txt", "a\\b\\three.md",
                                            "a\\b\\c\\four.md"}));
    EXPECT_EQ(listing.unsupported, 1) << "the docx";
    for (const auto& file : listing.files) {
        EXPECT_GT(file.size, 0);
        EXPECT_NE(file.modified, 0);
    }

    const auto missing = ListFolder(dir.path / "missing", 3, supported, "");
    EXPECT_TRUE(missing.files.empty());
    EXPECT_EQ(missing.unsupported, 0);
}

}  // namespace
}  // namespace clinicavt::guidance
