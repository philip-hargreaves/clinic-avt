#include "adapters/diarisation/anchor_store.hpp"

#include <cstring>
#include <fstream>
#include <stdexcept>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// clang-format off
#include <windows.h>
#include <dpapi.h>
// clang-format on

#include "core/common/log.hpp"
#include "core/diarisation/embeddings.hpp"

namespace clinicavt::diar {

namespace detail {

namespace {

constexpr std::uint32_t kVersion = 3;
constexpr std::uint32_t kVersionWithoutModel = 2;
constexpr std::size_t kHeader = 24;  // version, dims, sessions, enrolled_at

}  // namespace

// Version 3 puts the model's length and bytes between the header and the sum
std::vector<std::uint8_t> SerializeAnchor(const AnchorRecord& record) {
    const auto dims = static_cast<std::uint32_t>(record.sum.size());
    const auto model_bytes = static_cast<std::uint32_t>(record.model.size());
    std::vector<std::uint8_t> plain(kHeader + 4 + model_bytes + record.sum.size() * 4);
    std::memcpy(plain.data(), &kVersion, 4);
    std::memcpy(plain.data() + 4, &dims, 4);
    std::memcpy(plain.data() + 8, &record.sessions, 8);
    std::memcpy(plain.data() + 16, &record.enrolled_at, 8);
    std::memcpy(plain.data() + kHeader, &model_bytes, 4);
    std::memcpy(plain.data() + kHeader + 4, record.model.data(), model_bytes);
    std::memcpy(plain.data() + kHeader + 4 + model_bytes, record.sum.data(), record.sum.size() * 4);
    return plain;
}

std::optional<AnchorRecord> ParseAnchor(std::span<const std::uint8_t> plain) {
    if (plain.size() < kHeader) return std::nullopt;
    std::uint32_t version = 0, dims = 0;
    std::memcpy(&version, plain.data(), 4);
    std::memcpy(&dims, plain.data() + 4, 4);
    AnchorRecord record;
    std::memcpy(&record.sessions, plain.data() + 8, 8);
    std::memcpy(&record.enrolled_at, plain.data() + 16, 8);
    std::size_t offset = kHeader;
    if (version == kVersion) {
        std::uint32_t model_bytes = 0;
        if (plain.size() < offset + 4) return std::nullopt;
        std::memcpy(&model_bytes, plain.data() + offset, 4);
        offset += 4;
        if (plain.size() - offset < model_bytes) return std::nullopt;
        record.model.assign(reinterpret_cast<const char*>(plain.data() + offset), model_bytes);
        offset += model_bytes;
    } else if (version != kVersionWithoutModel) {
        return std::nullopt;
    }
    if (plain.size() - offset != static_cast<std::size_t>(dims) * 4) return std::nullopt;
    record.sum.resize(dims);
    std::memcpy(record.sum.data(), plain.data() + offset, static_cast<std::size_t>(dims) * 4);
    return record;
}

}  // namespace detail

namespace {

std::filesystem::path TempPath(const std::filesystem::path& path) {
    return path.string() + ".tmp";
}

// A crash leaves the old file or the new one whole
void WriteReplacing(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    const auto temp = TempPath(path);
    const HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("anchor write failed");
    DWORD written = 0;
    const bool saved =
        WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != 0 &&
        written == bytes.size() && FlushFileBuffers(file) != 0;
    CloseHandle(file);
    if (!saved || MoveFileExW(temp.c_str(), path.c_str(),
                              MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        throw std::runtime_error("anchor write failed");
    }
}

}  // namespace

AnchorStore::AnchorStore(const std::filesystem::path& root, std::string model)
    : path_(root / "anchor.bin"), model_(std::move(model)) {
    Load();
}

std::optional<std::vector<float>> AnchorStore::Anchor() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (record_.sum.empty()) return std::nullopt;
    std::vector<float> anchor = record_.sum;
    Normalise(anchor);
    return anchor;
}

AnchorStatus AnchorStore::Status() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    AnchorStatus status;
    status.sessions = record_.sessions;
    status.enrolled_at = record_.enrolled_at;
    if (record_.enrolled_at != 0) {
        status.origin = AnchorOrigin::kEnrolled;
    } else if (!record_.sum.empty()) {
        status.origin = AnchorOrigin::kAccrued;
    }
    return status;
}

void AnchorStore::Accrue(std::span<const float> voiceprint) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (voiceprint.empty()) return;
    if (record_.sum.size() != voiceprint.size()) {
        record_ = {};
        record_.sum.assign(voiceprint.size(), 0.0f);
    }
    for (std::size_t i = 0; i < voiceprint.size(); ++i) record_.sum[i] += voiceprint[i];
    record_.sessions += 1;
    record_.model = model_;
    Save();
}

void AnchorStore::Replace(std::span<const float> voiceprint, std::uint64_t enrolled_at) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (voiceprint.empty()) return;
    record_ = {};
    record_.sum.assign(voiceprint.begin(), voiceprint.end());
    for (float& x : record_.sum) x *= static_cast<float>(kEnrolWeight);
    record_.enrolled_at = enrolled_at;
    record_.model = model_;
    Save();
}

void AnchorStore::Clear() {
    const std::lock_guard<std::mutex> lock(mutex_);
    Erase();
}

void AnchorStore::Erase() {
    record_ = {};
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
    std::filesystem::remove(TempPath(path_), ignored);
}

void AnchorStore::Load() {
    // A save cut short leaves its temporary file, which is never read
    std::error_code ignored;
    std::filesystem::remove(TempPath(path_), ignored);
    std::ifstream in(path_, std::ios::binary);
    if (!in.is_open()) return;
    const std::vector<std::uint8_t> wrapped((std::istreambuf_iterator<char>(in)),
                                            std::istreambuf_iterator<char>());
    in.close();
    DATA_BLOB blob_in{static_cast<DWORD>(wrapped.size()),
                      const_cast<std::uint8_t*>(wrapped.data())};
    DATA_BLOB blob_out{};
    if (!CryptUnprotectData(&blob_in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                            &blob_out)) {
        log::Printf("clinicavt-engine: anchor unreadable, starting fresh\n");
        return;
    }
    const auto record = detail::ParseAnchor({blob_out.pbData, blob_out.cbData});
    SecureZeroMemory(blob_out.pbData, blob_out.cbData);
    LocalFree(blob_out.pbData);
    if (!record.has_value()) {
        log::Printf("clinicavt-engine: anchor format mismatch, starting fresh\n");
        return;
    }
    if (!model_.empty() && !record->model.empty() && record->model != model_) {
        log::Printf("clinicavt-engine: anchor made by another embedding model, starting fresh\n");
        Erase();
        return;
    }
    record_ = *record;
    // A version 2 print is taken as the current model's and rewritten with its name
    if (record_.model.empty() && !model_.empty() && !record_.sum.empty()) {
        record_.model = model_;
        try {
            Save();
        } catch (const std::exception& e) {
            log::Printf("clinicavt-engine: anchor not rewritten: %s\n", e.what());
        }
    }
}

void AnchorStore::Save() const {
    std::vector<std::uint8_t> plain = detail::SerializeAnchor(record_);
    DATA_BLOB blob_in{static_cast<DWORD>(plain.size()), plain.data()};
    DATA_BLOB blob_out{};
    const bool sealed = CryptProtectData(&blob_in, L"clinicavt clinician anchor", nullptr, nullptr,
                                         nullptr, CRYPTPROTECT_UI_FORBIDDEN, &blob_out) != 0;
    SecureZeroMemory(plain.data(), plain.size());
    if (!sealed) throw std::runtime_error("CryptProtectData failed for the anchor");
    const std::vector<std::uint8_t> wrapped(blob_out.pbData, blob_out.pbData + blob_out.cbData);
    LocalFree(blob_out.pbData);
    WriteReplacing(path_, wrapped);
}

}  // namespace clinicavt::diar
