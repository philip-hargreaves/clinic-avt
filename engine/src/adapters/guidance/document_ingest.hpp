#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "adapters/guidance/document_index.hpp"
#include "adapters/guidance/ingest_host.hpp"
#include "adapters/guidance/retriever.hpp"
#include "adapters/interfaces/document_ingest.hpp"
#include "adapters/system/recycle_bin.hpp"

namespace clinicavt::guidance {

inline constexpr const char* kReadMe = "Instructions.txt";

// Creates a missing folder and copies in the shipped guidelines; returns the
// count. Never refills an existing folder, so user deletions stay deleted
std::size_t SeedGuidelines(const std::filesystem::path& folder,
                           const std::filesystem::path& shipped);

// Watches the guidelines folder. A scan every few seconds picks up new and
// changed files, drops documents whose files are gone, and queues work for the
// ingest thread, which reads via the host, embeds one unit at a time between
// note searches, and publishes ready documents as one snapshot. The index
// under root is a cache and lists without an embedder
class DocumentIngest : public IDocumentIngest {
   public:
    using Discard = std::function<void(const std::filesystem::path&)>;

    DocumentIngest(Retriever& retriever, std::filesystem::path folder,
                   const std::filesystem::path& root, std::function<bool()> busy,
                   const std::filesystem::path& host_exe = {}, HostLimits host_limits = {},
                   Discard discard = system::RecycleFile,
                   std::chrono::milliseconds scan_every = std::chrono::seconds(3));
    ~DocumentIngest() override;

    Accepted Add(const std::vector<std::filesystem::path>& paths) override;
    Listing List() override;
    void Remove(std::int64_t id) override;
    std::size_t RemoveAll() override;
    PageRender Render(std::int64_t id, int page, std::int64_t chunk) override;
    std::filesystem::path Path(std::int64_t id) override;
    void SetListener(std::function<void(const IngestProgress&)> progress,
                     std::function<void(const DocumentInfo&)> document) override;

   private:
    struct Seen {
        std::int64_t size = 0;
        std::int64_t modified = 0;
    };
    struct Queued {
        std::int64_t id;
        std::string path;
    };

    // Syncs index to folder. Files in `fresh` skip the settle wait
    void Scan(const std::set<std::string>& fresh = {});
    void Work();
    void Index(const Queued& item);
    std::vector<Page> Extract(const std::vector<std::uint8_t>& bytes);
    void Keep(const std::filesystem::path& path);
    void Publish();
    void Notify(const DocumentInfo& info);
    void Progress(std::int64_t id, const char* phase, int done, int total);
    std::filesystem::path Absolute(const std::string& relative) const;
    bool Supported(const std::string& mime) const;
    bool Cancelled();

    Retriever& retriever_;
    std::filesystem::path folder_;
    std::filesystem::path scratch_;
    std::function<bool()> busy_;
    IngestHost host_;
    bool has_host_ = false;
    Discard discard_;
    std::chrono::milliseconds scan_every_;

    // Lock order: store_mutex_ first, then mutex_ or listener_mutex_
    std::mutex store_mutex_;
    DocumentIndex index_;
    bool found_ = true;
    int unsupported_ = 0;
    std::map<std::string, Seen>
        pending_;  // new or changed files, indexed once unchanged for a scan

    std::mutex scratch_mutex_;
    std::deque<std::filesystem::path> drawn_;

    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    std::deque<Queued> queue_;
    std::int64_t current_ = 0;
    bool cancel_ = false;
    bool stop_ = false;

    std::mutex listener_mutex_;
    std::function<void(const IngestProgress&)> on_progress_;
    std::function<void(const DocumentInfo&)> on_document_;
};

}  // namespace clinicavt::guidance
