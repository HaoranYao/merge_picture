// bar_detector.cpp

#include "bar_detector.h"

#include <algorithm>

namespace picmerge {

namespace {
// Tolerance per fingerprint byte. Each bin is a mean over ~100 bytes, so
// real sticky UI elements average out to very close values across JPEG
// recompressions — but the red "88VIP" promo banner and the nav bar
// contain small anti-aliased glyphs whose per-bin mean can still wobble by
// 5-8 counts across separately-compressed screenshots. 8 is empirically
// sufficient for those, while the jump to unrelated scroll content is
// much larger (typically >50 per byte), so we're nowhere near false
// positives.
constexpr int kBarTol = 8;

bool row_matches_all(const std::vector<RowSignatures>& sigs, int y) {
    const uint8_t* ref = sigs[0].row(y);
    for (size_t i = 1; i < sigs.size(); ++i) {
        if (!rows_match(ref, sigs[i].row(y), kBarTol)) return false;
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
