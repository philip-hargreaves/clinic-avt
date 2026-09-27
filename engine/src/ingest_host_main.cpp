// Reads a PDF from stdin and writes its pages as JSON, or one page as a BMP,
// on stdout. Its own process, so a parser fault ends one document and never
// the engine
#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdint>
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
#include <fpdf_text.h>
#include <fpdfview.h>
#include <werapi.h>
#include <windows.h>

#include <nlohmann/json.hpp>

#include "adapters/guidance/ingest_exit.hpp"
#include "adapters/system/exe_paths.hpp"
#include "core/common/strings.hpp"
#include "core/common/utf8.hpp"

namespace {

using json = nlohmann::json;

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

// Code points arrive as UTF-16 units. A lone surrogate becomes U+FFFD
void AppendUtf8(std::string& out, unsigned int unit, unsigned int& high) {
    if (unit >= 0xD800 && unit <= 0xDBFF) {
        high = unit;
        return;
    }
    unsigned int cp = unit;
    if (unit >= 0xDC00 && unit <= 0xDFFF) {
        cp = high != 0 ? 0x10000 + ((high - 0xD800) << 10) + (unit - 0xDC00) : 0xFFFD;
    } else if (high != 0) {
        clinicavt::utf8::Encode(out, 0xFFFD);
    }
    high = 0;
    clinicavt::utf8::Encode(out, cp);
}

struct Line {
    std::string text;
    double left = 0, top = 0, right = 0, bottom = 0;
    bool any = false;
};

// Characters in content order. A line ends at a generated break or when the
// next character sits clear of the line's band. Boxes are in points with the
// page's rotation applied, origin top left
json PageJson(FPDF_DOCUMENT doc, int index) {
    FPDF_PAGE page = FPDF_LoadPage(doc, index);
    json out{{"width", 0}, {"height", 0}, {"lines", json::array()}};
    if (page == nullptr) return out;
    const float width = FPDF_GetPageWidthF(page);
    const float height = FPDF_GetPageHeightF(page);
    out["width"] = width;
    out["height"] = height;

    json lines = json::array();
    FPDF_TEXTPAGE text = FPDFText_LoadPage(page);
    if (text != nullptr) {
        Line line;
        unsigned int high = 0;
        const auto flush = [&] {
            line.text = std::string(clinicavt::strings::Trim(line.text));
            if (line.any && !line.text.empty()) {
                lines.push_back(
                    {{"text", line.text}, {"box", {line.left, line.top, line.right, line.bottom}}});
            }
            line = Line{};
            high = 0;
        };
        const int scale_x = static_cast<int>(std::lround(width * 10));
        const int scale_y = static_cast<int>(std::lround(height * 10));
        const int count = FPDFText_CountChars(text);
        for (int i = 0; i < count; ++i) {
            const unsigned int unit = FPDFText_GetUnicode(text, i);
            if (unit == '\n' || unit == '\r') {
                flush();
                continue;
            }
            // The spaces PDFium puts between words have no glyph and sit anywhere
            // on the line, so only drawn glyphs shape the box
            double l = 0, r = 0, b = 0, t = 0;
            const bool drawn = unit > ' ' && unit != 0xA0 && !FPDFText_IsGenerated(text, i) &&
                               FPDFText_GetCharBox(text, i, &l, &r, &b, &t);
            if (drawn) {
                int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
                FPDF_PageToDevice(page, 0, 0, scale_x, scale_y, 0, l, t, &x0, &y0);
                FPDF_PageToDevice(page, 0, 0, scale_x, scale_y, 0, r, b, &x1, &y1);
                const double left = std::min(x0, x1) / 10.0;
                const double right = std::max(x0, x1) / 10.0;
                const double top = std::min(y0, y1) / 10.0;
                const double bottom = std::max(y0, y1) / 10.0;
                if (line.any) {
                    const double band = std::max(bottom - top, line.bottom - line.top);
                    const double centre = (top + bottom) / 2;
                    if (centre < line.top - band * 0.3 || centre > line.bottom + band * 0.3) {
                        flush();
                    }
                }
                if (!line.any) {
                    line.left = left;
                    line.top = top;
                    line.right = right;
                    line.bottom = bottom;
                    line.any = true;
                } else {
                    line.left = std::min(line.left, left);
                    line.top = std::min(line.top, top);
                    line.right = std::max(line.right, right);
                    line.bottom = std::max(line.bottom, bottom);
                }
            }
            AppendUtf8(line.text, unit, high);
        }
        flush();
        FPDFText_ClosePage(text);
    }
    out["lines"] = std::move(lines);
    FPDF_ClosePage(page);
    return out;
}

int WriteOut(const void* data, std::size_t size) {
    if (size > kOutputCap) return ingest_exit::kOutputBound;
    _setmode(_fileno(stdout), _O_BINARY);
    std::fwrite(data, 1, size, stdout);
    std::fflush(stdout);
    return ingest_exit::kOk;
}

int Extract(FPDF_DOCUMENT doc) {
    json pages = json::array();
    const int count = FPDF_GetPageCount(doc);
    std::size_t line_total = 0;
    for (int i = 0; i < count; ++i) {
        pages.push_back(PageJson(doc, i));
        line_total += pages.back()["lines"].size();
    }
    const std::string out = json{{"pages", std::move(pages)}}.dump();
    std::fprintf(stderr, "clinicavt-ingest-host: %d pages, %zu lines\n", count, line_total);
    return WriteOut(out.data(), out.size());
}

void Put32(std::vector<unsigned char>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<unsigned char>(v >> (8 * i)));
}

// One page as a 32-bit top-down BMP, white behind the content
int Render(FPDF_DOCUMENT doc, int index, int dpi) {
    if (index < 0 || index >= FPDF_GetPageCount(doc)) return ingest_exit::kBadPage;
    FPDF_PAGE page = FPDF_LoadPage(doc, index);
    if (page == nullptr) return ingest_exit::kBadPage;
    const int width =
        std::max(1, static_cast<int>(std::lround(FPDF_GetPageWidthF(page) * dpi / 72)));
    const int height =
        std::max(1, static_cast<int>(std::lround(FPDF_GetPageHeightF(page) * dpi / 72)));
    FPDF_BITMAP bitmap = FPDFBitmap_Create(width, height, 0);
    if (bitmap == nullptr) {
        FPDF_ClosePage(page);
        return ingest_exit::kOutputBound;
    }
    FPDFBitmap_FillRect(bitmap, 0, 0, width, height, 0xFFFFFFFF);
    FPDF_RenderPageBitmap(bitmap, page, 0, 0, width, height, 0, FPDF_ANNOT);
    const auto* buffer = static_cast<const unsigned char*>(FPDFBitmap_GetBuffer(bitmap));
    const int stride = FPDFBitmap_GetStride(bitmap);
    const std::size_t row = static_cast<std::size_t>(width) * 4;
    std::vector<unsigned char> out;
    out.reserve(54 + row * height);
    out.push_back('B');
    out.push_back('M');
    Put32(out, static_cast<std::uint32_t>(54 + row * height));
    Put32(out, 0);
    Put32(out, 54);
    Put32(out, 40);
    Put32(out, static_cast<std::uint32_t>(width));
    Put32(out, static_cast<std::uint32_t>(-height));
    Put32(out, 1u | (32u << 16));
    Put32(out, 0);
    Put32(out, static_cast<std::uint32_t>(row * height));
    Put32(out, 2835);
    Put32(out, 2835);
    Put32(out, 0);
    Put32(out, 0);
    for (int y = 0; y < height; ++y) {
        const auto* line = buffer + static_cast<std::size_t>(y) * stride;
        out.insert(out.end(), line, line + row);
    }
    FPDFBitmap_Destroy(bitmap);
    FPDF_ClosePage(page);
    std::fprintf(stderr, "clinicavt-ingest-host: page %d at %d dpi, %d x %d\n", index + 1, dpi,
                 width, height);
    return WriteOut(out.data(), out.size());
}

}  // namespace

int main(int argc, char* argv[]) {
    // A crash writes no dump of the document
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

    FPDF_LIBRARY_CONFIG config{};
    config.version = 2;
    FPDF_InitLibraryWithConfig(&config);
    FPDF_DOCUMENT doc = FPDF_LoadMemDocument(bytes.data(), static_cast<int>(bytes.size()), nullptr);
    if (doc == nullptr) {
        const auto error = FPDF_GetLastError();
        FPDF_DestroyLibrary();
        return error == FPDF_ERR_PASSWORD || error == FPDF_ERR_SECURITY ? ingest_exit::kPassword
                                                                        : ingest_exit::kCannotOpen;
    }
    const int code = extract
                         ? Extract(doc)
                         : Render(doc, std::atoi(argv[2]), std::clamp(std::atoi(argv[3]), 36, 300));
    FPDF_CloseDocument(doc);
    FPDF_DestroyLibrary();
    return code;
}
