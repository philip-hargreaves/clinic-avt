#include "adapters/storage/chunk_cipher.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace clinicavt::store {
namespace {

std::vector<std::uint8_t> SampleChunk() {
    std::vector<std::uint8_t> plain(64 * 1024);
    for (std::size_t i = 0; i < plain.size(); ++i) {
        plain[i] = static_cast<std::uint8_t>(i * 31);
    }
    return plain;
}

// Empty is real: a loss-only commit seals zero frames
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

    // Domains share a key and count seq from zero, so their IVs must stay disjoint
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

}  // namespace
}  // namespace clinicavt::store
