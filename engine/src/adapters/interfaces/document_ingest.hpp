#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace clinicavt::guidance {

enum class DocumentState { kIndexing, kReady, kFailed, kRemoved };

// Names used by the index and the wire
inline const char* DocumentStateName(DocumentState state) {
    switch (state) {
        case DocumentState::kReady:
            return "ready";
        case DocumentState::kFailed:
            return "failed";
        case DocumentState::kRemoved:
            return "removed";
        case DocumentState::kIndexing:
            break;
    }
    return "indexing";
}

inline std::optional<DocumentState> DocumentStateFrom(std::string_view name) {
    if (name == "indexing") return DocumentState::kIndexing;
    if (name == "ready") return DocumentState::kReady;
    if (name == "failed") return DocumentState::kFailed;
    if (name == "removed") return DocumentState::kRemoved;
    return std::nullopt;
}

// A document in the guidelines folder, one per distinct content
struct DocumentInfo {
    std::int64_t id = 0;
    std::string name;    // the file's stem
    std::string path;    // relative to the folder, the shortest when several hold it
    std::string sha256;  // of the file, hex
    std::string mime;
    DocumentState state = DocumentState::kIndexing;
    std::string error;  // a reason code
    std::string added_at;
    std::string indexed_at;
    std::int64_t bytes = 0;
    int pages = 0;
    int pages_without_text = 0;
    std::int64_t chunks = 0;
};

// Folder state as of the last scan
struct Listing {
    std::filesystem::path folder;
    bool found = true;    // false when the folder and its parent were out of reach
    int unsupported = 0;  // files of other types
    std::vector<DocumentInfo> documents;
};

struct IngestProgress {
    std::int64_t id = 0;
    std::string phase;  // reading, preparing, paused
    int done = 0;
    int total = 0;
};

struct Skipped {
    std::string path;
    std::string reason;  // unsupported, noSpace, unreadable
};

struct Accepted {
    std::vector<DocumentInfo> documents;
    std::vector<Skipped> skipped;
};

// One page drawn for the page view: a BMP under the scratch folder
struct PageRender {
    std::filesystem::path path;
    int width = 0;
    int height = 0;
    int pages = 0;      // the document's page count
    std::string boxes;  // the chunk's line boxes as page fractions, JSON
};

// The clinician's guidelines folder, searched by content. Files are read and
// embedded on a dedicated thread, paused during a consultation, and dropped
// when deleted. Listeners get progress and every state change, including removal
class IDocumentIngest {
   public:
    virtual ~IDocumentIngest() = default;
    // Copies files into the folder
    virtual Accepted Add(const std::vector<std::filesystem::path>& paths) = 0;
    virtual Listing List() = 0;
    // Sends every file holding the document to the Recycle Bin
    virtual void Remove(std::int64_t id) = 0;
    // Removes every document; returns the count
    virtual std::size_t RemoveAll() = 0;
    virtual PageRender Render(std::int64_t id, int page, std::int64_t chunk) = 0;
    // Path in the guidelines folder, for the PDF viewer
    virtual std::filesystem::path Path(std::int64_t id) = 0;
    virtual void SetListener(std::function<void(const IngestProgress&)> progress,
                             std::function<void(const DocumentInfo&)> document) = 0;
};

}  // namespace clinicavt::guidance
