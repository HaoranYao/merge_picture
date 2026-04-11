// sticky_header.cpp

#include "sticky_header.h"

#include <algorithm>

namespace picmerge {

namespace {
// Slightly tighter than bar tolerance: sticky headers should render
// pixel-identically between two screenshots (same app, same state).
constexpr int kStickyTol = 4;
} // namespace

int detect_sticky_header(const RowSignatures& prev,
                         const RowSignatures& next,
                         int top_bar_height,
                         int bottom_bar_height,
                         int max_sticky) {
    if (prev.height != next.height) return 0;
    const int H = prev.height;
    const int limit = std::min(max_sticky, H - top_bar_height - bottom_bar_height);
    if (limit <= 0) return 0;

    int s = 0;
    while (s < limit) {
        const int y = top_bar_height + s;
        if (!rows_match(prev.row(y), next.row(y), kStickyTol)) break;
        ++s;
    }
    return s;
}

} // namespace picmerge
