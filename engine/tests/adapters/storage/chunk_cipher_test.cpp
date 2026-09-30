#include "adapters/storage/chunk_cipher.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "adapters/storage/sqlite_store_rows.hpp"

namespace clinicavt::store {
namespace {

std::vector<std::uint8_t> SampleChunk() {
    std::vector<std::uint8_t> plain(64 * 1024);
    for (std::size_t i = 0; i < plain.size(); ++i) {
        plain[i] = static_cast<std::uint8_t>(i * 31);
    }
    return plain;
}

// A loss-only commit seals an empty payload
TEST(ChunkCipher, SealedPayloadsRoundTripAlsoEmptyAndThroughTheWrappedKey) {
    const ChunkCipher cipher = ChunkCipher::Generate();
    const ChunkCipher stored = ChunkCipher::FromWrapped(cipher.Wrapped());
    for (const auto& plain : {SampleChunk(), std::vector<std::uint8_t>{}}) {
        SCOPED_TRACE(plain.empty() ? "empty" : "64 KiB");
        const auto sealed = cipher.Seal(Domain::kAudio, "session-a", 7, plain);
        EXPECT_EQ(sealed.size(), plain.size() + 16) << "ciphertext and tag";
        if (!plain.empty()) {
            EXPECT_FALSE(std::equal(plain.begin(), plain.end(), sealed.begin()))
                << "sealed as plaintext";
        }
        EXPECT_EQ(cipher.Open(Domain::kAudio, "session-a", 7, sealed), plain);
        EXPECT_EQ(stored.Open(Domain::kAudio, "session-a", 7, sealed), plain)
            << "the unwrapped key opens it";
    }
    EXPECT_THROW(ChunkCipher::FromWrapped(std::vector<std::uint8_t>(64, 0xAB)), std::runtime_error);
}

TEST(ChunkCipher, APayloadOpensOnlyUnderItsOwnKeyDomainSessionAndSequence) {
    const ChunkCipher cipher = ChunkCipher::Generate();
    const ChunkCipher other = ChunkCipher::Generate();
    const auto plain = SampleChunk();
    const auto sealed = cipher.Seal(Domain::kAudio, "session-a", 7, plain);

    // Domains share a key, so the same sequence in two domains must give different IVs
    const auto as_turn = cipher.Seal(Domain::kTurns, "session-a", 7, plain);
    EXPECT_NE(as_turn, sealed) << "same seq in different domains must differ";
    EXPECT_EQ(cipher.Open(Domain::kTurns, "session-a", 7, as_turn), plain);

    auto tampered = sealed;
    tampered[100] ^= 0x01;
    struct Case {
        const char* what;
        const ChunkCipher* key;
        Domain domain;
        const char* session;
        std::uint64_t seq;
        std::vector<std::uint8_t> payload;
    };
    const std::vector<Case> cases = {
        {"a flipped byte", &cipher, Domain::kAudio, "session-a", 7, tampered},
        {"shorter than the tag", &cipher, Domain::kAudio, "session-a", 0,
         std::vector<std::uint8_t>(15)},
        {"renumbered", &cipher, Domain::kAudio, "session-a", 8, sealed},
        {"moved to another session", &cipher, Domain::kAudio, "session-b", 7, sealed},
        {"a turn opened as audio", &cipher, Domain::kAudio, "session-a", 7, as_turn},
        {"another key", &other, Domain::kAudio, "session-a", 7, sealed},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.what);
        EXPECT_THROW(c.key->Open(c.domain, c.session, c.seq, c.payload), std::runtime_error);
    }
}

std::string Hex(std::span<const std::uint8_t> bytes) {
    std::string hex;
    for (const std::uint8_t b : bytes) hex += std::format("{:02x}", b);
    return hex;
}

// Stored payloads open only under the domain they were sealed with, so each number and each
// kind's domain is fixed. A fixed key, context and sequence give fixed bytes per domain
TEST(ChunkCipher, EveryDomainAndKindSealsAsItAlwaysHas) {
    const std::vector<std::uint8_t> salt(16, 0x5a);
    const ChunkCipher cipher = ChunkCipher::FromPassword("known answer", salt, 1000);
    const std::string plain = "sealed text";
    const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(plain.data()),
                                              plain.size());
    const std::vector<std::string> expected = {
        "7956c542f34bf9750d4c48bc4e7e95ae66d19da3e0beaa20565885",
        "da822b79af474decc99bdac78969b9f9e1e3fced7f7296ff7ceb5f",
        "a6539bb7096f73ef096df5694c574153043b2f3ca88846982f7e54",
        "af1b4b74db3284a950adf907163c9dd2fc3d413aec29399d4b707f",
        "ea1803d6baee89b94b209576eaff32c5b5a89daa0ac1133bda3e49",
        "8a0e09f0ca55bdc333d1c6e5e18d4a94b96025c133553592b0030d",
        "e108f7e22bee7bd6c0b371a784b00cd62e841604c24a797a856b8d",
        "e630730b20199335353c8b3f518482baef9edefea923e28dc1734c",
        "a645727e39aa71a29cc5d4c0d0a00bd3f800f9855cac5529932274",
        "3488584a82683da3253a2e546645f55791eac483f977548fbdff3b",
    };
    for (int domain = 0; domain < 10; ++domain) {
        SCOPED_TRACE(domain);
        EXPECT_EQ(Hex(cipher.Seal(static_cast<Domain>(domain), "0123456789abcdef0123456789abcdef",
                                  42, bytes)),
                  expected[static_cast<std::size_t>(domain)]);
    }

    struct Kind {
        DocumentKind kind;
        const char* name;
        Domain domain;
    };
    const Kind kinds[] = {
        {DocumentKind::kNote, "note", Domain::kNote},
        {DocumentKind::kPatient, "patient", Domain::kPatient},
        {DocumentKind::kTranslation, "translation", Domain::kTranslation},
        {DocumentKind::kLabel, "label", Domain::kLabel},
        {DocumentKind::kSummary, "summary", Domain::kSummary},
        {DocumentKind::kReflection, "reflection", Domain::kReflection},
        {DocumentKind::kGuidance, "guidance", Domain::kGuidance},
    };
    ASSERT_EQ(std::size(kinds), kDocumentKinds.size());
    for (const Kind& k : kinds) {
        SCOPED_TRACE(k.name);
        EXPECT_STREQ(rows::SpecFor(k.kind).name, k.name);
        EXPECT_EQ(rows::SpecFor(k.kind).domain, k.domain);
    }
}

}  // namespace
}  // namespace clinicavt::store
