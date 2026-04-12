// bar_detector.cpp

#include "bar_detector.h"

#include <algorithm>

namespace picmerge {

namespace {
// Per-row L1 sum threshold for bar detection.
//
// We use the total L1 distance across all kSigBins fingerprint bytes rather
// than a strict per-bin maximum. This is more robust to JPEG recompression
// artifacts in fixed UI elements that contain anti-aliased text (e.g. the
// "已选: Pocket 3标准版" selection row in JD, or the "88VIP" promo banner in
// Taobao): one or two bins may wobble by 10–15 counts while the rest are
// near-zero, but the row is still visually identical across screenshots.
//
// Genuine scroll-content transitions show diffs of 50+ per bin across
// many bins (total L1 >> 500), so a threshold of 200 leaves a 2.5× safety
// margin against false positives while comfortably absorbing JPEG noise.
constexpr int kBarL1Thresh = 200;  // sum of |a[k]-b[k]| across 16 bins

bool row_matches_all(const std::vector<RowSignatures>& sigs, int y) {
    const uint8_t* ref = sigs[0].row(y);
    for (size_t i = 1; i < sigs.size(); ++i) {
        if (row_l1(ref, sigs[i].row(y)) > kBarL1Thresh) return false;
    }
    return true;
}
} // namespace

FixedBars detect_fixed_bars(const std::vector<RowSignatures>& sigs,
                            double max_fraction) {
    FixedBars bars;
    if (sigs.empty()) return bars;

    const int H = sigs.front().height;
    if (H <= 0) return bars;
    for (const auto& s : sigs) {
        if (s.height != H) return bars;
    }

    const int cap = static_cast<int>(H * max_fraction);

    int top = 0;
    while (top < cap) {
        if (!row_matches_all(sigs, top)) break;
        ++top;
    }

    int bot = 0;
    while (bot < cap) {
        const int y = H - 1 - bot;
        if (y < top) break;
        if (!row_matches_all(sigs, y)) break;
        ++bot;
    }

    bars.top_height    = top;
    bars.bottom_height = bot;
    return bars;
}

} // namespace picmerge
