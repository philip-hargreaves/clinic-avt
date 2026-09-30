#include "core/archive/record_rules.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace clinicavt::archive {
namespace {

store::SessionRecord Valid() {
    store::SessionRecord record;
    record.id = "0123456789abcdef0123456789abcdef";
    record.started_at = "2026-09-01T09:00:00Z";
    record.ended_at = "2026-09-01T09:12:30Z";
    record.sample_rate = 16000;
    store::Document note;
    note.text = "Swollen left elbow.";
    note.revision = 3;
    note.generated_at = "2026-09-01T09:13:00Z";
    record.documents.push_back({store::DocumentKind::kNote, note});
    store::Document label;
    label.text = "Elbow";
    label.revision = 1;
    label.edited_at = "2026-09-02T10:00:00Z";
    record.documents.push_back({store::DocumentKind::kLabel, label});
    return record;
}

TEST(RecordRules, OnlyTheFormIso8601WritesIsATimestamp) {
    EXPECT_TRUE(IsIso8601("2026-09-01T09:00:00Z"));
    for (const char* text :
         {"", "2026-09-01", "2026-09-01T09:00:00", "2026-09-01T09:00:00.5Z", "2026-09-01 09:00:00Z",
          "2026-9-1T9:00:00Z", "2026-02-30T09:00:00Z", "2026-09-01T09:00:00+01:00", "not a time"}) {
        EXPECT_FALSE(IsIso8601(text)) << text;
    }
}

TEST(RecordRules, ARecordTheStoreCouldHaveWrittenIsValid) {
    EXPECT_TRUE(ValidRecord(Valid()));

    store::SessionRecord bare = Valid();
    bare.documents.clear();
    EXPECT_TRUE(ValidRecord(bare)) << "a session with no documents";
}

TEST(RecordRules, EachFieldTheStoreReliesOnIsChecked) {
    using Change = std::function<void(store::SessionRecord&)>;
    struct Row {
        const char* name;
        Change change;
    };
    const std::vector<Row> rows = {
        {"a short id", [](auto& r) { r.id = "0123"; }},
        {"an upper-case id", [](auto& r) { r.id = "0123456789ABCDEF0123456789ABCDEF"; }},
        {"a non-hex id", [](auto& r) { r.id = "0123456789abcdef0123456789abcdeg"; }},
        {"no start", [](auto& r) { r.started_at.clear(); }},
        {"no end", [](auto& r) { r.ended_at.clear(); }},
        {"a local end time", [](auto& r) { r.ended_at = "2026-09-01T09:12:30"; }},
        {"no sample rate", [](auto& r) { r.sample_rate = 0; }},
        {"a negative sample rate", [](auto& r) { r.sample_rate = -16000; }},
        {"an unknown kind",
         [](auto& r) { r.documents[0].kind = static_cast<store::DocumentKind>(99); }},
        {"a kind twice", [](auto& r) { r.documents[1].kind = store::DocumentKind::kNote; }},
        {"revision 0", [](auto& r) { r.documents[0].document.revision = 0; }},
        {"a revision past the headroom",
         [](auto& r) {
             r.documents[0].document.revision = std::numeric_limits<std::int64_t>::max();
         }},
        {"a bad generated time", [](auto& r) { r.documents[0].document.generated_at = "today"; }},
        {"a bad edited time", [](auto& r) { r.documents[1].document.edited_at = "2026-09-02"; }},
    };
    for (const auto& row : rows) {
        store::SessionRecord record = Valid();
        row.change(record);
        EXPECT_FALSE(ValidRecord(record)) << row.name;
    }
}

TEST(RecordRules, TheHighestRevisionLeavesRoomToIncrement) {
    store::SessionRecord record = Valid();
    record.documents[0].document.revision =
        std::numeric_limits<std::int64_t>::max() - (std::int64_t{1} << 32);
    EXPECT_TRUE(ValidRecord(record));
    record.documents[0].document.revision += 1;
    EXPECT_FALSE(ValidRecord(record));
}

}  // namespace
}  // namespace clinicavt::archive
