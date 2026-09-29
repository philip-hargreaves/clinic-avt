// Reads a PDF on stdin and writes its pages as JSON, or one page as BMP, to stdout. It runs as a
// separate process so a parser crash only fails one document
#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
#include <werapi.h>
// clang-format on

#include "adapters/guidance/ingest_exit.hpp"
#include "adapters/pdf/pdfium_reader.hpp"
#include "adapters/system/exe_paths.hpp"
#include "adapters/system/stderr_log.hpp"
#include "core/common/log.hpp"

namespace {

namespace ingest_exit = clinicavt::guidance::ingest_exit;
constexpr std::size_t kOutputCap = 64u << 20;

std::vector<unsigned char> ReadStdin() {
    _setmode(_fileno(stdin), _O_BINARY);
    std::vector<unsigned char> bytes;
    unsigned char buffer[1 << 16];
    for (;;) {
        const auto count = std::fread(buffer, 1, sizeof buffer, stdin);
        if (count == 0) break;
        bytes.insert(bytes.end(), buffer, buffer + count);
    }
    return bytes;
}

int WriteOut(const void* data, std::size_t size) {
    if (size > kOutputCap) return ingest_exit::kOutputBound;
    _setmode(_fileno(stdout), _O_BINARY);
    std::fwrite(data, 1, size, stdout);
    std::fflush(stdout);
    return ingest_exit::kOk;
}

int Extract(const clinicavt::pdf::PdfiumReader& reader) {
    int pages = 0;
    std::size_t lines = 0;
    const std::string out = reader.ExtractJson(pages, lines);
    clinicavt::log::Printf("clinicavt-ingest-host: %d pages, %zu lines\n", pages, lines);
    return WriteOut(out.data(), out.size());
}

int Render(const clinicavt::pdf::PdfiumReader& reader, int index, int dpi) {
    std::vector<unsigned char> out;
    int width = 0;
    int height = 0;
    const int code = reader.RenderBmp(index, dpi, out, width, height);
    if (code != ingest_exit::kOk) return code;
    clinicavt::log::Printf("clinicavt-ingest-host: page %d at %d dpi, %d x %d\n", index + 1, dpi,
                           width, height);
    return WriteOut(out.data(), out.size());
}

}  // namespace

int main(int argc, char* argv[]) {
    clinicavt::system::LogToStderr();
    // No crash dump, so the document never lands on disk
    WerAddExcludedApplication(clinicavt::system::kIngestHostExe, FALSE);
    const bool extract = argc == 2 && std::strcmp(argv[1], "extract") == 0;
    const bool render = argc == 4 && std::strcmp(argv[1], "render") == 0;
    if (!extract && !render) {
        std::fprintf(stderr,
                     "usage: clinicavt_ingest_host extract < document.pdf\n"
                     "       clinicavt_ingest_host render <page> <dpi> < document.pdf\n");
        return ingest_exit::kBadArgs;
    }
    const auto bytes = ReadStdin();
    if (bytes.empty() || bytes.size() > static_cast<std::size_t>(INT_MAX))
        return ingest_exit::kCannotOpen;

    const clinicavt::pdf::PdfiumReader reader(bytes);
    if (reader.Status() != ingest_exit::kOk) return reader.Status();
    return extract ? Extract(reader)
                   : Render(reader, std::atoi(argv[2]), std::clamp(std::atoi(argv[3]), 36, 300));
}
