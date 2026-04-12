// bar_detector.cpp

#include "bar_detector.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>

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

// Check whether row `y` qualifies as a fixed-bar row using majority voting.
//
// Instead of requiring ALL images to agree, we allow up to `max_outliers`
// images to differ. This handles the case where a temporary UI overlay
// (e.g. a floating popup, dismissible banner, or live-streaming badge)
// covers part of the bar in a minority of screenshots.
//
// Returns true if there exists a reference image such that at least
// (N - max_outliers) images (including the reference) agree with it.
bool row_is_bar(const std::vector<RowSignatures>& sigs, int y,
                int max_outliers) {
    const int N = static_cast<int>(sigs.size());
    for (int ref = 0; ref < N; ++ref) {
        int disagree = 0;
        const uint8_t* rrow = sigs[static_cast<size_t>(ref)].row(y);
        for (int i = 0; i < N; ++i) {
            if (i == ref) continue;
            if (row_l1(rrow, sigs[static_cast<size_t>(i)].row(y)) > kBarL1Thresh) {
                ++disagree;
                if (disagree > max_outliers) break;
            }
        }
        if (disagree <= max_outliers) return true;
    }
    return false;
}

// Find the image whose bar rows best represent the majority.
// Returns the image index with the lowest total L1 against all others
// across the bar region [y_begin, y_end).
int find_best_ref(const std::vector<RowSignatures>& sigs,
                  int y_begin, int y_end) {
    const int N = static_cast<int>(sigs.size());
    if (N <= 1) return 0;

    int best_img = 0;
    int64_t best_total = std::numeric_limits<int64_t>::max();

    for (int ref = 0; ref < N; ++ref) {
        int64_t total = 0;
        for (int y = y_begin; y < y_end; ++y) {
            const uint8_t* rrow = sigs[static_cast<size_t>(ref)].row(y);
            for (int i = 0; i < N; ++i) {
                if (i == ref) continue;
                total += row_l1(rrow, sigs[static_cast<size_t>(i)].row(y));
            }
        }
        if (total < best_total) {
            best_total = total;
            best_img = ref;
        }
    }
    return best_img;
}

} // namespace

FixedBars detect_fixed_bars(const std::vector<RowSignatures>& sigs,
                            double max_fraction) {
    FixedBars bars;
    if (sigs.empty()) return bars;

    const int N = static_cast<int>(sigs.size());
    const int H = sigs.front().height;
    if (H <= 0) return bars;
    for (const auto& s : sigs) {
        if (s.height != H) return bars;
    }

    const int cap = static_cast<int>(H * max_fraction);

    // Top bar: strict matching (all images must agree). The top bar is the
    // OS status bar; sticky headers that appear mid-scroll are content, not
    // bar, and should NOT be absorbed even if the majority of images show
    // them.
    int top = 0;
    while (top < cap) {
        if (!row_is_bar(sigs, top, /*max_outliers=*/0)) break;
        ++top;
    }

    // Bottom bar: allow up to N/3 outlier images. Temporary UI overlays
    // (floating banners, live-streaming badges, dismissible promos) may
    // cover the bar in a minority of screenshots; majority voting sees
    // through them.
    const int max_outliers = (N <= 2) ? 0 : N / 3;

    int bot = 0;
    while (bot < cap) {
        const int y = H - 1 - bot;
        if (y < top) break;
        if (!row_is_bar(sigs, y, max_outliers)) break;
        ++bot;
    }

    bars.top_height    = top;
    bars.bottom_height = bot;

    // Pick the image whose bar rows best match the majority.
    if (top > 0)
        bars.top_ref = find_best_ref(sigs, 0, top);
    if (bot > 0)
        bars.bot_ref = find_best_ref(sigs, H - bot, H);

    // Debug: print L1 values around the bottom-bar boundary.
    if (const char* env = std::getenv("PICMERGE_DEBUG_BARS"); env && *env) {
        const int start = std::max(0, H - bot - 10);
        const int end   = std::min(H, H - bot + 5);
        for (int y = start; y < end; ++y) {
            std::fprintf(stderr, "  row %4d (from_bot %4d):", y, H - 1 - y);
            for (size_t i = 1; i < sigs.size(); ++i) {
                int l1 = row_l1(sigs[0].row(y), sigs[i].row(y));
                std::fprintf(stderr, " L1[0-%zu]=%d", i, l1);
            }
            std::fprintf(stderr, "%s\n", (y >= H - bot) ? "  *bar*" : "");
        }
        if (bot > 0)
            std::fprintf(stderr, "  bot_ref = img[%d]\n", bars.bot_ref);
    }

    return bars;
}

} // namespace picmerge
