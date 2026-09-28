#include "adapters/archive/archive_file.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "adapters/storage/chunk_cipher.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// clang-format off
#include <windows.h>
#include <dpapi.h>
// clang-format on

namespace clinicavt::archive {
namespace {

using store::DocumentKind;
using store::SessionRecord;

// The lowest count a reader accepts, so tests derive in microseconds
constexpr std::uint32_t kLowIterations = 1000;
constexpr const char* kPassword = "correct horse battery staple";

struct TempDir {
    std::filesystem::path path;

    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("clinicavt-archive-" +
                std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "-" +
                ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }

    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

std::filesystem::path PartialOf(const std::filesystem::path& target) {
    std::filesystem::path partial = target;
    partial += ".partial";
    return partial;
}

std::vector<std::uint8_t> ReadBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void WriteBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

store::RecordDocument Doc(DocumentKind kind, std::string text, std::string language,
                          std::string generated_at, std::string edited_at, std::int64_t revision) {
    store::RecordDocument d;
    d.kind = kind;
    d.document.text = std::move(text);
    d.document.language = std::move(language);
    d.document.generated_at = std::move(generated_at);
    d.document.edited_at = std::move(edited_at);
    d.document.revision = revision;
    return d;
}

// Every field and every document kind, non-ASCII text, a revision past 2^53 and a cleared
// consultation holding only its appraisal entry. The golden v1 file was written from these, so
// changing them fails that test
std::vector<SessionRecord> SampleRecords() {
    SessionRecord full;
    full.id = "0123456789abcdef0123456789abcdef";
    full.started_at = "2026-07-01T09:00:00Z";
    full.ended_at = "2026-07-01T09:12:30Z";
    full.sample_rate = 16000;
    full.device_id = "{0.0.1.00000000}.{6a1d0e2c-3b8f-4c55-9e0a-1f2d3c4b5a69}";
    full.device_name = "Microphone (USB Audio)";
    full.lost_frames = 320;
    full.turns = {{0, 48000, "Doctor", "What brings you in today?"},
                  {48000, 96000, "Patient",
                   "A na\xC3\xAFve question: 5 \xC2\xB5g in \xE6\x9D\xB1\xE4\xBA\xAC?"}};
    auto note = Doc(DocumentKind::kNote, "Plan: review in 2 weeks, 5 \xC2\xB5g daily.", "en",
                    "2026-07-01T09:13:05Z", "2026-07-01T09:20:00Z", (std::int64_t{1} << 61) + 7);
    note.document.style = "soap";
    note.document.detail = "detailed";
    full.documents = {
        note,
        Doc(DocumentKind::kPatient, "What we talked about today.", "en", "2026-07-01T09:13:40Z", "",
            3),
        Doc(DocumentKind::kTranslation,
            "\xD8\xA2\xD9\xBE \xDA\xA9\xD8\xA7 \xD8\xB4\xDA\xA9\xD8\xB1\xDB\x8C\xDB\x81", "ur",
            "2026-07-01T09:14:00Z", "", 2),
        Doc(DocumentKind::kLabel, "Knee pain review", "en", "2026-07-01T09:13:05Z", "", 1),
        Doc(DocumentKind::kSummary, "A knee review that went well.", "en", "2026-07-01T09:15:00Z",
            "", 4),
        Doc(DocumentKind::kReflection, "Next time ask about sleep.", "en", "2026-07-01T09:16:00Z",
            "2026-07-02T08:00:00Z", 5),
        Doc(DocumentKind::kGuidance, R"({"version":1,"shown":[],"searched":[],"noteRevision":7})",
            "en", "2026-07-01T09:13:10Z", "", 9),
    };

    // A cleared label is a row with no text, kept by presence
    SessionRecord cleared;
    cleared.id = "fedcba9876543210fedcba9876543210";
    cleared.started_at = "2026-08-15T14:30:00Z";
    cleared.ended_at = "2026-08-15T14:41:00Z";
    cleared.sample_rate = 16000;
    cleared.documents = {
        Doc(DocumentKind::kLabel, "", "en", "2026-08-15T14:42:00Z", "2026-08-16T10:00:00Z", 2),
        Doc(DocumentKind::kReflection, "Kept for appraisal.", "en", "2026-08-15T14:45:00Z", "", 1),
    };
    return {full, cleared};
}

Manifest SampleManifest(std::size_t consultations) {
    Manifest m;
    m.created_at = "2026-09-27T10:00:00Z";
    m.from = "2026-07-01T00:00:00Z";
    m.to = "2026-10-01T00:00:00Z";
    m.consultations = consultations;
    m.transcripts = true;
    m.app_version = "1.4.0";
    return m;
}

void ExpectSame(const Manifest& got, const Manifest& want) {
    EXPECT_EQ(got.version, want.version);
    EXPECT_EQ(got.created_at, want.created_at);
    EXPECT_EQ(got.from, want.from);
    EXPECT_EQ(got.to, want.to);
    EXPECT_EQ(got.consultations, want.consultations);
    EXPECT_EQ(got.transcripts, want.transcripts);
    EXPECT_EQ(got.app_version, want.app_version);
}

void ExpectSame(const SessionRecord& got, const SessionRecord& want) {
    SCOPED_TRACE(want.id);
    EXPECT_EQ(got.id, want.id);
    EXPECT_EQ(got.started_at, want.started_at);
    EXPECT_EQ(got.ended_at, want.ended_at);
    EXPECT_EQ(got.sample_rate, want.sample_rate);
    EXPECT_EQ(got.device_id, want.device_id);
    EXPECT_EQ(got.device_name, want.device_name);
    EXPECT_EQ(got.lost_frames, want.lost_frames);
    ASSERT_EQ(got.turns.size(), want.turns.size());
    for (std::size_t i = 0; i < want.turns.size(); ++i) {
        EXPECT_EQ(got.turns[i].first_frame, want.turns[i].first_frame);
        EXPECT_EQ(got.turns[i].frame_count, want.turns[i].frame_count);
        EXPECT_EQ(got.turns[i].speaker, want.turns[i].speaker);
        EXPECT_EQ(got.turns[i].text, want.turns[i].text);
    }
    ASSERT_EQ(got.documents.size(), want.documents.size());
    for (std::size_t i = 0; i < want.documents.size(); ++i) {
        const auto& g = got.documents[i];
        const auto& w = want.documents[i];
        EXPECT_EQ(g.kind, w.kind);
        EXPECT_EQ(g.document.text, w.document.text);
        EXPECT_EQ(g.document.language, w.document.language);
        EXPECT_EQ(g.document.style, w.document.style);
        EXPECT_EQ(g.document.detail, w.document.detail);
        EXPECT_EQ(g.document.generated_at, w.document.generated_at);
        EXPECT_EQ(g.document.edited_at, w.document.edited_at);
        EXPECT_EQ(g.document.revision, w.document.revision);
    }
}

void WriteBackup(const std::filesystem::path& path, const std::string& password,
                 const std::vector<SessionRecord>& records) {
    ArchiveFileSink sink(path, password, kLowIterations);
    sink.Begin(SampleManifest(records.size()));
    for (const auto& r : records) sink.Add(r);
    sink.Commit();
}

std::vector<SessionRecord> Drain(IArchiveSource& source) {
    std::vector<SessionRecord> records;
    while (auto record = source.Next()) records.push_back(std::move(*record));
    return records;
}

std::optional<ArchiveCode> CodeOf(const std::function<void()>& run) {
    try {
        run();
    } catch (const ArchiveError& e) {
        return e.Code();
    }
    return std::nullopt;
}

// Written decomposed and opened composed, so the key comes from the NFC form
TEST(ArchiveFile, EveryFieldRoundTripsAndAnAccentTypedEitherWayOpensIt) {
    TempDir dir;
    const auto path = dir.path / "backup.clinicavt";
    const std::string decomposed = "cafe\xCC\x81 au lait 2026";
    const std::string composed = "caf\xC3\xA9 au lait 2026";
    const auto records = SampleRecords();
    WriteBackup(path, decomposed, records);

    EXPECT_FALSE(std::filesystem::exists(PartialOf(path)));
    const auto bytes = ReadBytes(path);
    const std::string clear = "What brings you in today?";
    EXPECT_EQ(std::search(bytes.begin(), bytes.end(), clear.begin(), clear.end()), bytes.end())
        << "a transcript in the clear";

    ArchiveFileSource source(path, composed);
    ExpectSame(source.GetManifest(), SampleManifest(records.size()));
    for (const char* pass : {"first read", "after rewind"}) {
        SCOPED_TRACE(pass);
        const auto read = Drain(source);
        ASSERT_EQ(read.size(), records.size());
        for (std::size_t i = 0; i < records.size(); ++i) ExpectSame(read[i], records[i]);
        EXPECT_FALSE(source.Next()) << "the end stays the end";
        source.Rewind();
    }
}

struct Span {
    std::size_t at;    // the length prefix
    std::size_t size;  // prefix and sealed bytes
};

// The layout, read independently of the reader: 30-byte header, then u32 LE length + sealed
std::vector<Span> Records(const std::vector<std::uint8_t>& bytes) {
    std::vector<Span> spans;
    for (std::size_t at = 30; at + 4 <= bytes.size();) {
        std::size_t length = 0;
        for (int i = 3; i >= 0; --i) length = length << 8 | bytes[at + i];
        spans.push_back({at, 4 + length});
        at += 4 + length;
    }
    return spans;
}

void PutLe32(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) bytes[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

// Every way a file can be wrong fails with its reason, at open or before the last record, so a
// restore that reads the whole file first never writes from one
TEST(ArchiveFile, DamagedBackupsAreRefusedWithTheirReason) {
    TempDir dir;
    const auto good = dir.path / "good.clinicavt";
    WriteBackup(good, kPassword, SampleRecords());
    const auto original = ReadBytes(good);
    const auto spans = Records(original);
    ASSERT_EQ(spans.size(), 3u) << "manifest and two consultations";
    const Span manifest = spans[0];
    const Span first = spans[1];
    const Span last = spans[2];

    using Bytes = std::vector<std::uint8_t>;
    struct Case {
        const char* what;
        std::function<void(Bytes&)> damage;
        std::string password;
        ArchiveCode code;
    };
    const std::vector<Case> cases = {
        {"another password", [](Bytes&) {}, "correct horse battery stapler",
         ArchiveCode::kWrongPassword},
        {"a flipped byte in the header's salt", [](Bytes& b) { b[20] ^= 0x01; }, kPassword,
         ArchiveCode::kWrongPassword},
        {"a flipped byte in the manifest", [&](Bytes& b) { b[manifest.at + 10] ^= 0x01; },
         kPassword, ArchiveCode::kWrongPassword},
        {"a flipped byte in a consultation", [&](Bytes& b) { b[last.at + 10] ^= 0x01; }, kPassword,
         ArchiveCode::kDamaged},
        {"cut at a record boundary", [&](Bytes& b) { b.resize(last.at); }, kPassword,
         ArchiveCode::kDamaged},
        {"cut inside a record", [&](Bytes& b) { b.resize(last.at + 10); }, kPassword,
         ArchiveCode::kDamaged},
        {"a stray byte after the end", [](Bytes& b) { b.push_back(0); }, kPassword,
         ArchiveCode::kDamaged},
        {"the last record again",
         [&](Bytes& b) {
             b.insert(b.end(), original.begin() + last.at, original.begin() + last.at + last.size);
         },
         kPassword, ArchiveCode::kDamaged},
        {"two records swapped",
         [&](Bytes& b) {
             b.resize(first.at);
             b.insert(b.end(), original.begin() + last.at, original.begin() + last.at + last.size);
             b.insert(b.end(), original.begin() + first.at,
                      original.begin() + first.at + first.size);
         },
         kPassword, ArchiveCode::kDamaged},
        {"a length past the bound", [&](Bytes& b) { PutLe32(b, last.at, 0xFFFFFFFF); }, kPassword,
         ArchiveCode::kDamaged},
        {"a length shorter than a tag", [&](Bytes& b) { PutLe32(b, first.at, 3); }, kPassword,
         ArchiveCode::kDamaged},
        {"iterations past the bound", [](Bytes& b) { PutLe32(b, 10, 10000001); }, kPassword,
         ArchiveCode::kDamaged},
        {"a foreign magic", [](Bytes& b) { b[0] = 'X'; }, kPassword, ArchiveCode::kNotABackup},
        {"shorter than a header", [](Bytes& b) { b.resize(10); }, kPassword,
         ArchiveCode::kNotABackup},
        {"a newer version", [](Bytes& b) { b[8] = 2; }, kPassword, ArchiveCode::kNewerVersion},
    };
    const auto damaged = dir.path / "damaged.clinicavt";
    for (const Case& c : cases) {
        SCOPED_TRACE(c.what);
        Bytes bytes = original;
        c.damage(bytes);
        WriteBytes(damaged, bytes);
        const auto code = CodeOf([&] {
            ArchiveFileSource source(damaged, c.password);
            Drain(source);
        });
        ASSERT_TRUE(code.has_value()) << "read to the end";
        EXPECT_EQ(*code, c.code) << ArchiveError::Name(*code);
    }
    EXPECT_EQ(CodeOf([&] { ArchiveFileSource(dir.path / "absent.clinicavt", kPassword); }),
              ArchiveCode::kReadFailed);
}

// Counted in code points after NFC, so neither multi-byte letters nor decomposed accents pass
// a short password
TEST(ArchiveFile, APasswordUnderEightCharactersIsRefusedBeforeAnythingIsWritten) {
    TempDir dir;
    const auto path = dir.path / "backup.clinicavt";
    std::string seven_accents;
    std::string four_decomposed;
    for (int i = 0; i < 7; ++i) seven_accents += "\xC3\xA9";
    for (int i = 0; i < 4; ++i) four_decomposed += "e\xCC\x81";
    for (const std::string& weak : {std::string("shorter"), seven_accents, four_decomposed}) {
        EXPECT_EQ(CodeOf([&] { ArchiveFileSink(path, weak, kLowIterations); }),
                  ArchiveCode::kWeakPassword);
    }
    EXPECT_FALSE(std::filesystem::exists(path));
    EXPECT_FALSE(std::filesystem::exists(PartialOf(path)));

    EXPECT_EQ(CodeOf([&] { ArchiveFileSink(path, "eight ch", kLowIterations); }), std::nullopt);
    EXPECT_FALSE(std::filesystem::exists(PartialOf(path))) << "abandoned, so removed";
}

TEST(ArchiveFile, AFailedBackUpLeavesNoPartialAndKeepsTheExistingFile) {
    TempDir dir;
    const auto target = dir.path / "backup.clinicavt";
    const std::vector<std::uint8_t> previous = {'p', 'r', 'e', 'v', 'i', 'o', 'u', 's'};
    const auto records = SampleRecords();
    const auto abandon = [&] {
        ArchiveFileSink sink(target, kPassword, kLowIterations);
        sink.Begin(SampleManifest(records.size()));
        sink.Add(records[0]);
    };

    struct Case {
        const char* what;
        bool existing;
        std::function<void()> run;
    };
    const std::vector<Case> cases = {
        {"abandoned before commit", false, abandon},
        {"abandoned over an existing file", true, abandon},
        {"fewer consultations than the manifest", true,
         [&] {
             ArchiveFileSink sink(target, kPassword, kLowIterations);
             sink.Begin(SampleManifest(records.size()));
             sink.Add(records[0]);
             EXPECT_EQ(CodeOf([&] { sink.Commit(); }), ArchiveCode::kWriteFailed);
         }},
        {"the existing file cannot be replaced", true,
         [&] {
             SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_READONLY);
             ArchiveFileSink sink(target, kPassword, kLowIterations);
             sink.Begin(SampleManifest(records.size()));
             for (const auto& r : records) sink.Add(r);
             EXPECT_EQ(CodeOf([&] { sink.Commit(); }), ArchiveCode::kWriteFailed);
             SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_NORMAL);
         }},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.what);
        std::filesystem::remove(target);
        if (c.existing) WriteBytes(target, previous);
        c.run();
        EXPECT_FALSE(std::filesystem::exists(PartialOf(target)));
        if (c.existing) {
            EXPECT_EQ(ReadBytes(target), previous);
        } else {
            EXPECT_FALSE(std::filesystem::exists(target));
        }
    }
}

// A cipher holding exactly these key bytes, through the store's own unwrap, as a cipher never
// exposes its key
store::ChunkCipher WithKey(const std::array<std::uint8_t, 32>& key) {
    DATA_BLOB in{static_cast<DWORD>(key.size()), const_cast<BYTE*>(key.data())};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                          &out)) {
        throw std::runtime_error("CryptProtectData failed");
    }
    std::vector<std::uint8_t> wrapped(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return store::ChunkCipher::FromWrapped(wrapped);
}

// RFC 7914 section 11 (first 32 bytes), and RFC 6070's inputs under SHA-256
TEST(ArchiveFile, KeysArePbkdf2HmacSha256OfThePasswordBytes) {
    struct Vector {
        std::string password;
        std::string salt;
        std::uint32_t iterations;
        std::array<std::uint8_t, 32> key;
    };
    const std::vector<Vector> vectors = {
        {"passwd", "salt", 1, {0x55, 0xac, 0x04, 0x6e, 0x56, 0xe3, 0x08, 0x9f, 0xec, 0x16, 0x91,
                               0xc2, 0x25, 0x44, 0xb6, 0x05, 0xf9, 0x41, 0x85, 0x21, 0x6d, 0xde,
                               0x04, 0x65, 0xe6, 0x8b, 0x9d, 0x57, 0xc2, 0x0d, 0xac, 0xbc}},
        {"password", "salt", 4096, {0xc5, 0xe4, 0x78, 0xd5, 0x92, 0x88, 0xc8, 0x41,
                                    0xaa, 0x53, 0x0d, 0xb6, 0x84, 0x5c, 0x4c, 0x8d,
                                    0x96, 0x28, 0x93, 0xa0, 0x01, 0xce, 0x4e, 0x11,
                                    0xa4, 0x96, 0x38, 0x73, 0xaa, 0x98, 0x13, 0x4a}},
    };
    const std::vector<std::uint8_t> plain = {'k', 'n', 'o', 'w', 'n'};
    for (const Vector& v : vectors) {
        SCOPED_TRACE(v.password);
        const auto derived = store::ChunkCipher::FromPassword(
            v.password,
            std::span(reinterpret_cast<const std::uint8_t*>(v.salt.data()), v.salt.size()),
            v.iterations);
        const auto sealed = derived.Seal(store::Domain::kArchive, "header", 3, plain);
        EXPECT_EQ(WithKey(v.key).Open(store::Domain::kArchive, "header", 3, sealed), plain);
    }
}

// Composed on purpose: the fixture pins the KDF input as the UTF-8 of the NFC form
constexpr const char* kGoldenPassword = "na\xC3\xAFve-\xE6\x9D\xB1\xE4\xBA\xAC-backup";

std::filesystem::path GoldenPath() {
    return std::filesystem::path(CLINICAVT_ARCHIVE_FIXTURE_DIR) / "v1.clinicavt";
}

bool WritingGolden() {
    char* value = nullptr;
    const bool set = _dupenv_s(&value, nullptr, "CLINICAVT_WRITE_GOLDEN") == 0 &&
                     value != nullptr && std::string(value) == "1";
    std::free(value);
    return set;
}

// Run once, by hand, to make the fixture; never again for v1:
//   $env:CLINICAVT_WRITE_GOLDEN=1; engine_tests --gtest_filter=ArchiveFile.WriteGoldenV1
TEST(ArchiveFile, WriteGoldenV1) {
    if (!WritingGolden()) GTEST_SKIP() << "writes the fixture only with CLINICAVT_WRITE_GOLDEN=1";
    ASSERT_FALSE(std::filesystem::exists(GoldenPath())) << "v1 is written once and kept";
    std::filesystem::create_directories(GoldenPath().parent_path());
    WriteBackup(GoldenPath(), kGoldenPassword, SampleRecords());
}

// Every later build must open every v1 file. This one was written on another computer under
// another Windows user, so it also proves a backup moves between machines
TEST(ArchiveFile, TheV1GoldenBackupStillOpensWithEveryField) {
    ArchiveFileSource source(GoldenPath(), kGoldenPassword);
    const auto records = SampleRecords();
    ExpectSame(source.GetManifest(), SampleManifest(records.size()));
    const auto read = Drain(source);
    ASSERT_EQ(read.size(), records.size());
    for (std::size_t i = 0; i < records.size(); ++i) ExpectSame(read[i], records[i]);
}

}  // namespace
}  // namespace clinicavt::archive
