#include "core/archive/record_rules.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <sstream>
#include <vector>

#include "core/common/iso8601.hpp"

namespace clinicavt::archive {

namespace {

// Leaves a restored revision room to count further rewrites
constexpr std::int64_t kMaxRevision =
    std::numeric_limits<std::int64_t>::max() - (std::int64_t{1} << 32);

bool IsSessionId(const std::string& id) {
    return id.size() == 32 && std::ranges::all_of(id, [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

bool IsOptionalIso8601(const std::string& text) {
    return text.empty() || IsIso8601(text);
}

}  // namespace

bool IsIso8601(const std::string& text) {
    std::istringstream in(text);
    std::chrono::sys_seconds at;
    in >> std::chrono::parse("%FT%TZ", at);
    return !in.fail() && Iso8601(at) == text;
}

bool ValidRecord(const store::SessionRecord& record) {
    if (!IsSessionId(record.id) || !IsIso8601(record.started_at) || !IsIso8601(record.ended_at) ||
        record.sample_rate <= 0) {
        return false;
    }
    std::vector<store::DocumentKind> seen;
    for (const store::RecordDocument& entry : record.documents) {
        if (std::ranges::find(store::kDocumentKinds, entry.kind) == store::kDocumentKinds.end() ||
            std::ranges::find(seen, entry.kind) != seen.end()) {
            return false;
        }
        seen.push_back(entry.kind);
        const store::Document& document = entry.document;
        if (document.revision < 1 || document.revision > kMaxRevision ||
            !IsOptionalIso8601(document.generated_at) || !IsOptionalIso8601(document.edited_at)) {
            return false;
        }
    }
    return true;
}

}  // namespace clinicavt::archive
