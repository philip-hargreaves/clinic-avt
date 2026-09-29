#include "adapters/pdf/pdfium_reader.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fpdf_text.h>
#include <fpdfview.h>

#include <nlohmann/json.hpp>

#include "adapters/guidance/ingest_exit.hpp"
#include "core/common/strings.hpp"
#include "core/common/utf8.hpp"

namespace clinicavt::pdf {

namespace {

using json = nlohmann::json;
namespace ingest_exit = guidance::ingest_exit;

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
        utf8::Encode(out, 0xFFFD);
    }
    high = 0;
    utf8::Encode(out, cp);
}

struct Line {
    std::string text;
    double left = 0, top = 0, right = 0, bottom = 0;
    bool any = false;
};

// Characters in content order. A line ends at a generated break or when the next character is
// outside the line band. Boxes are in points, with page rotation applied and the origin top left
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
            line.text = std::string(strings::Trim(line.text));
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
            // PDFium's inter-word spaces have no glyph and arbitrary positions, so only
            // drawn glyphs count toward the box
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

void Put32(std::vector<unsigned char>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<unsigned char>(v >> (8 * i)));
}

FPDF_DOCUMENT Doc(void* doc) {
    return static_cast<FPDF_DOCUMENT>(doc);
}

}  // namespace

PdfiumReader::PdfiumReader(const std::vector<unsigned char>& bytes) : status_(ingest_exit::kOk) {
    FPDF_LIBRARY_CONFIG config{};
    config.version = 2;
    FPDF_InitLibraryWithConfig(&config);
    doc_ = FPDF_LoadMemDocument(bytes.data(), static_cast<int>(bytes.size()), nullptr);
    if (doc_ == nullptr) {
        const auto error = FPDF_GetLastError();
        status_ = error == FPDF_ERR_PASSWORD || error == FPDF_ERR_SECURITY
                      ? ingest_exit::kPassword
                      : ingest_exit::kCannotOpen;
    }
}

PdfiumReader::~PdfiumReader() {
    if (doc_ != nullptr) FPDF_CloseDocument(Doc(doc_));
    FPDF_DestroyLibrary();
}

int PdfiumReader::Status() const {
    return status_;
}

std::string PdfiumReader::ExtractJson(int& pages, std::size_t& lines) const {
    json all = json::array();
    pages = FPDF_GetPageCount(Doc(doc_));
    lines = 0;
    for (int i = 0; i < pages; ++i) {
        all.push_back(PageJson(Doc(doc_), i));
        lines += all.back()["lines"].size();
    }
    return json{{"pages", std::move(all)}}.dump();
}

int PdfiumReader::RenderBmp(int index, int dpi, std::vector<unsigned char>& out, int& width,
                            int& height) const {
    FPDF_DOCUMENT doc = Doc(doc_);
    if (index < 0 || index >= FPDF_GetPageCount(doc)) return ingest_exit::kBadPage;
    FPDF_PAGE page = FPDF_LoadPage(doc, index);
    if (page == nullptr) return ingest_exit::kBadPage;
    width = std::max(1, static_cast<int>(std::lround(FPDF_GetPageWidthF(page) * dpi / 72)));
    height = std::max(1, static_cast<int>(std::lround(FPDF_GetPageHeightF(page) * dpi / 72)));
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
    out.clear();
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
    return ingest_exit::kOk;
}

}  // namespace clinicavt::pdf
