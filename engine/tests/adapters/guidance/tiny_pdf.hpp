#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace clinicavt::guidance::fixture {

// A one-page PDF with the lines set in Helvetica, offsets computed here
inline std::vector<std::uint8_t> TinyPdf(const std::vector<std::string>& lines) {
    std::string content = "BT /F1 12 Tf 72 770 Td ";
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i) content += "0 -16 Td ";
        content += "(" + lines[i] + ") Tj ";
    }
    content += "ET";
    const std::vector<std::string> objects = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        // NOLINTNEXTLINE(bugprone-suspicious-missing-comma) one object over two lines
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents 4 0 R"
        " /Resources << /Font << /F1 5 0 R >> >> >>",
        "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "\nendstream",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    };
    std::string pdf = "%PDF-1.4\n";
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const auto xref = pdf.size();
    pdf += "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (const auto offset : offsets) {
        char entry[32];
        std::snprintf(entry, sizeof entry, "%010zu 00000 n \n", offset);
        pdf += entry;
    }
    pdf += "trailer\n<< /Size " + std::to_string(objects.size() + 1) +
           " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return {pdf.begin(), pdf.end()};
}

}  // namespace clinicavt::guidance::fixture
