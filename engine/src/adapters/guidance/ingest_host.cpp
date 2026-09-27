#include "adapters/guidance/ingest_host.hpp"

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <thread>
#include <utility>

#include "adapters/guidance/ingest_exit.hpp"
#include "adapters/system/child_process.hpp"

namespace clinicavt::guidance {
namespace {

using json = nlohmann::json;

constexpr std::size_t kMaxPages = 10'000;
constexpr std::size_t kMaxLinesPerPage = 100'000;
constexpr int kMaxPixels = 10'000;
constexpr std::size_t kBmpHeader = 54;

struct Outcome {
    DWORD exit = 0;
    std::string output;
    bool timed_out = false;
    bool bounded = false;
};

void Pipe(system::UniqueHandle& read, system::UniqueHandle& write) {
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &attributes, 0)) {
        throw HostError("crashed", "ingest host pipe failed");
    }
    read.Reset(read_end);
    write.Reset(write_end);
}

Outcome RunHost(const std::filesystem::path& exe, const std::wstring& args,
                std::span<const std::uint8_t> input, const HostLimits& limits) {
    system::UniqueHandle in_read, in_write, out_read, out_write;
    Pipe(in_read, in_write);
    Pipe(out_read, out_write);
    SetHandleInformation(in_write.get(), HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_read.get(), HANDLE_FLAG_INHERIT, 0);
    // One process on a memory cap. The document goes in on stdin, the
    // pages come back on stdout
    system::ChildProcess child;
    try {
        child = system::ChildProcess::Spawn(exe, args,
                                            {.stdin_read = in_read.get(),
                                             .stdout_write = out_write.get(),
                                             .memory_cap = limits.memory_cap,
                                             .single_process = true});
    } catch (const std::exception&) {
        throw HostError("crashed", "ingest host failed to start");
    }
    in_read.Reset();
    out_write.Reset();

    Outcome outcome;
    std::thread writer([&] {
        std::size_t at = 0;
        while (at < input.size()) {
            DWORD written = 0;
            const auto count =
                static_cast<DWORD>(std::min<std::size_t>(input.size() - at, 1 << 16));
            if (!WriteFile(in_write.get(), input.data() + at, count, &written, nullptr)) break;
            at += written;
        }
        in_write.Reset();
    });
    std::thread reader([&] {
        char buffer[1 << 16];
        DWORD count = 0;
        while (ReadFile(out_read.get(), buffer, sizeof buffer, &count, nullptr) && count > 0) {
            if (outcome.output.size() + count > limits.output_cap) {
                outcome.bounded = true;
                child.Kill();
                break;
            }
            outcome.output.append(buffer, count);
        }
    });
    if (!child.WaitFor(static_cast<DWORD>(limits.timeout.count()))) {
        outcome.timed_out = true;
        child.Kill();
    }
    // The reader ends when the process is gone and its end of the pipe with it
    writer.join();
    reader.join();
    outcome.exit = child.ExitCode();
    return outcome;
}

float Fraction(const json& value, float whole) {
    if (!value.is_number() || whole <= 0) return 0;
    return std::clamp(static_cast<float>(value.get<double>() / whole), 0.0F, 1.0F);
}

// The host's JSON as pages, refused when a count or a size is out of range
std::vector<Page> PagesOf(json root) {
    if (!root.is_object() || !root["pages"].is_array() || root["pages"].size() > kMaxPages) {
        throw HostError("badOutput", "pages missing or too many");
    }
    std::vector<Page> pages;
    for (const auto& p : root["pages"]) {
        Page page;
        const auto width = p.value("width", 0.0);
        const auto height = p.value("height", 0.0);
        if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0) {
            throw HostError("badOutput", "page size out of range");
        }
        page.width = static_cast<float>(width);
        page.height = static_cast<float>(height);
        const auto& lines = p.at("lines");
        if (!lines.is_array() || lines.size() > kMaxLinesPerPage) {
            throw HostError("badOutput", "lines missing or too many");
        }
        for (const auto& l : lines) {
            const auto& box = l.at("box");
            if (!l.at("text").is_string() || !box.is_array() || box.size() != 4) {
                throw HostError("badOutput", "line shape");
            }
            PageLine line;
            line.text = l.at("text").get<std::string>();
            line.box = {Fraction(box[0], page.width), Fraction(box[1], page.height),
                        Fraction(box[2], page.width), Fraction(box[3], page.height)};
            if (line.box.right < line.box.left || line.box.bottom < line.box.top) {
                throw HostError("badOutput", "box inverted");
            }
            page.lines.push_back(std::move(line));
        }
        pages.push_back(std::move(page));
    }
    return pages;
}

// A missing key or a wrong type is bad output like any other
std::vector<Page> PagesFrom(const std::string& text) {
    try {
        return PagesOf(json::parse(text));
    } catch (const json::exception& e) {
        throw HostError("badOutput", e.what());
    }
}

std::uint32_t Read32(const std::string& s, std::size_t at) {
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | static_cast<unsigned char>(s[at + i]);
    return v;
}

// The host's BMP, refused unless its header and its size agree
Bitmap BitmapFrom(std::string output) {
    if (output.size() < kBmpHeader || output[0] != 'B' || output[1] != 'M') {
        throw HostError("badOutput", "not a bitmap");
    }
    Bitmap bitmap;
    bitmap.width = static_cast<int>(Read32(output, 18));
    bitmap.height = -static_cast<int>(Read32(output, 22));
    if (bitmap.width <= 0 || bitmap.height <= 0 || bitmap.width > kMaxPixels ||
        bitmap.height > kMaxPixels ||
        output.size() != kBmpHeader + static_cast<std::size_t>(bitmap.width) * bitmap.height * 4) {
        throw HostError("badOutput", "bitmap size out of range");
    }
    bitmap.bmp.assign(output.begin(), output.end());
    return bitmap;
}

}  // namespace

IngestHost::IngestHost(std::filesystem::path exe, HostLimits limits)
    : exe_(std::move(exe)), limits_(limits) {}

std::string IngestHost::Run(std::span<const std::uint8_t> document,
                            const std::wstring& args) const {
    const auto outcome = RunHost(exe_, args, document, limits_);
    if (outcome.timed_out) throw HostError("timeout", "ingest host ran out of time");
    if (outcome.bounded) throw HostError("outputBound", "ingest host wrote too much");
    switch (outcome.exit) {
        case ingest_exit::kOk:
            return outcome.output;
        case ingest_exit::kCannotOpen:
            throw HostError("cannotOpen", "not a PDF this reader can open");
        case ingest_exit::kPassword:
            throw HostError("password", "the PDF is password protected");
        case ingest_exit::kOutputBound:
            throw HostError("outputBound", "ingest host wrote too much");
        case ingest_exit::kBadPage:
            throw HostError("badPage", "no such page");
        default:
            throw HostError("crashed", "ingest host exited with " + std::to_string(outcome.exit));
    }
}

std::vector<Page> IngestHost::Extract(std::span<const std::uint8_t> document) const {
    return PagesFrom(Run(document, L"extract"));
}

Bitmap IngestHost::Render(std::span<const std::uint8_t> document, int page, int dpi) const {
    return BitmapFrom(
        Run(document, L"render " + std::to_wstring(page) + L" " + std::to_wstring(dpi)));
}

}  // namespace clinicavt::guidance
