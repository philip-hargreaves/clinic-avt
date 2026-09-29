#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace clinicavt::guidance {

struct ScanHit {
    std::size_t ord = 0;
    float cosine = 0;
};

// The k nearest rows of a row-major matrix of unit vectors to a unit query:
// one dot product per row, then a partial sort. Exact, no index
inline std::vector<ScanHit> Scan(const float* matrix, std::size_t rows, int dim, const float* query,
                                 int k) {
    std::vector<ScanHit> hits(rows);
    const auto d = static_cast<std::size_t>(dim);
    for (std::size_t r = 0; r < rows; ++r) {
        const float* v = matrix + r * d;
        float dot = 0;
        for (std::size_t i = 0; i < d; ++i) dot += v[i] * query[i];
        hits[r] = {r, dot};
    }
    const auto keep = std::min<std::size_t>(static_cast<std::size_t>(std::max(k, 0)), rows);
    std::partial_sort(hits.begin(), hits.begin() + static_cast<std::ptrdiff_t>(keep), hits.end(),
                      [](const ScanHit& a, const ScanHit& b) {
                          if (a.cosine != b.cosine) return a.cosine > b.cosine;
                          return a.ord < b.ord;
                      });
    hits.resize(keep);
    return hits;
}

}  // namespace clinicavt::guidance
