#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace clinicavt::pdf {

// Reads a PDF held in memory with PDFium. Only the ingest host links it, so a parser fault
// ends that process alone. Status and failures are ingest_exit codes
class PdfiumReader {
   public:
    // Starts PDFium for this object's lifetime and opens the document
    explicit PdfiumReader(const std::vector<unsigned char>& bytes);
    ~PdfiumReader();
    PdfiumReader(const PdfiumReader&) = delete;
    PdfiumReader& operator=(const PdfiumReader&) = delete;

    // kOk, or kPassword or kCannotOpen when the document did not open
    int Status() const;

    // Every page's size and text lines as JSON, with the counts for the log
    std::string ExtractJson(int& pages, std::size_t& lines) const;

    // Renders one page as a 32-bit top-down BMP, white behind the content. Returns kOk, kBadPage or
    // kOutputBound
    int RenderBmp(int index, int dpi, std::vector<unsigned char>& out, int& width,
                  int& height) const;

   private:
    void* doc_ = nullptr;  // FPDF_DOCUMENT
    int status_;
};

}  // namespace clinicavt::pdf
