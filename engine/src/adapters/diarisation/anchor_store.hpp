#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace clinicavt::diar {

enum class AnchorOrigin { kNone, kAccrued, kEnrolled };

struct AnchorStatus {
    AnchorOrigin origin = AnchorOrigin::kNone;
    std::uint64_t sessions = 0;     // consultations accrued since the print began
    std::uint64_t enrolled_at = 0;  // unix seconds, 0 when never enrolled
};

namespace detail {

// A running sum of unit voiceprints and where it came from
struct AnchorRecord {
    std::vector<float> sum;
    std::uint64_t sessions = 0;
    std::uint64_t enrolled_at = 0;
    std::string model;  // the embedding model, empty in a version 2 file
};

// Writes version 3. Reads versions 2 and 3, and nullopt for anything else
std::vector<std::uint8_t> SerializeAnchor(const AnchorRecord& record);
std::optional<AnchorRecord> ParseAnchor(std::span<const std::uint8_t> plain);

}  // namespace detail

// Clinician voiceprint in anchor.bin, DPAPI-protected. Seeded by enrolment or the first
// consultation, refined by each one after. A corrupt file, or a print from another embedding
// model, resets to empty
class AnchorStore {
   public:
    // An enrolment counts as this many consultations, so the first few
    // sessions refine it rather than replace it
    static constexpr std::uint64_t kEnrolWeight = 3;

    // model identifies the speaker embedding model. Empty when none is staged, and then a
    // stored print is kept whatever model made it
    explicit AnchorStore(const std::filesystem::path& root, std::string model = {});

    // Unit norm. Nullopt before any enrolment or consultation
    std::optional<std::vector<float>> Anchor() const;
    AnchorStatus Status() const;

    void Accrue(std::span<const float> voiceprint);
    // Seeds from an enrolment, discarding anything accrued
    void Replace(std::span<const float> voiceprint, std::uint64_t enrolled_at);
    void Clear();

   private:
    void Load();
    void Save() const;
    void Erase();

    mutable std::mutex mutex_;
    std::filesystem::path path_;
    std::string model_;
    detail::AnchorRecord record_;
};

}  // namespace clinicavt::diar
