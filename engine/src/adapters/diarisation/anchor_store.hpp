#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace clinicavt::diar {

enum class AnchorOrigin { kNone, kAccrued, kEnrolled };

struct AnchorStatus {
    AnchorOrigin origin = AnchorOrigin::kNone;
    std::uint64_t sessions = 0;     // consultations accrued since the print began
    std::uint64_t enrolled_at = 0;  // unix seconds, 0 when never enrolled
};

namespace detail {

// The plain anchor record: a running sum of unit voiceprints plus its provenance
struct AnchorRecord {
    std::vector<float> sum;
    std::uint64_t sessions = 0;
    std::uint64_t enrolled_at = 0;
};

// Version 2 layout. Anything else is refused, and the store starts fresh
std::vector<std::uint8_t> SerializeAnchor(const AnchorRecord& record);
std::optional<AnchorRecord> ParseAnchor(std::span<const std::uint8_t> plain);

}  // namespace detail

// Clinician voiceprint, DPAPI-protected. Seeded by enrolment or the first
// consultation, refined by each one after. A corrupt file resets to empty
class AnchorStore {
   public:
    // An enrolment counts as this many consultations, so the first few
    // sessions refine it rather than replace it
    static constexpr std::uint64_t kEnrolWeight = 3;

    explicit AnchorStore(const std::filesystem::path& root);

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

    mutable std::mutex mutex_;
    std::filesystem::path path_;
    detail::AnchorRecord record_;
};

}  // namespace clinicavt::diar
