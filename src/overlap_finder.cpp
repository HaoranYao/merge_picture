// overlap_finder.cpp
//
// 1D sliding-window search using L1 distance on per-row fingerprints.
// Never touches the 2D pixel buffer — this is the core PRD performance
// constraint.

#include "overlap_finder.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace picmerge {

namespace {

// Try one specific template position and return its best match.
OverlapResult match_at(const RowSignatures& prev,
                       const RowSignatures& next,
                       int prev_search_begin,
                       int prev_usable_end,
                       int next_template_start,
                       int next_usable_end) {
    OverlapResult r;
    r.template_start_in_next = next_template_start;

    const int next_content_height = next_usable_end - next_template_start;
    if (next_content_height <= 0) return r;

    int L = std::min(256, next_content_height / 3);
    if (L < 32) L = std::min(next_content_height, 32);
    if (L <= 0) return r;
    r.template_length = L;

    const int search_begin = prev_search_begin;
    const int search_end   = prev_usable_end - L;
    if (search_end < search_begin) return r;

    const uint8_t* tmpl = next.row(next_template_start);

    int64_t best_cost = std::numeric_limits<int64_t>::max();
    int64_t second_cost = std::numeric_limits<int64_t>::max();
    int     best_d = -1;

    for (int d = search_begin; d <= search_end; ++d) {
        const uint8_t* prow = prev.row(d);
        int64_t cost = 0;
        for (int k = 0; k < L; ++k) {
            cost += row_l1(prow + static_cast<size_t>(k) * kSigBins,
                           tmpl + static_cast<size_t>(k) * kSigBins);
        }
        if (cost < best_cost) {
            if (best_d < 0 || std::abs(d - best_d) > L / 2) {
                second_cost = best_cost;
            }
            best_cost = cost;
            best_d = d;
        } else if (cost < second_cost &&
                   (best_d < 0 || std::abs(d - best_d) > L / 2)) {
            second_cost = cost;
        }
    }

    r.best_cost        = static_cast<double>(best_cost);
    r.second_best_cost = static_cast<double>(second_cost);

    if (best_d < 0) return r;

    const double mean_per_row = static_cast<double>(best_cost) / static_cast<double>(L);
    if (mean_per_row < 100.0) {   // ~6 per bin on average
        r.ok = true;
        r.offset_in_prev = best_d;
    }
    return r;
}

} // namespace

OverlapResult find_overlap(const RowSignatures& prev,
                           const RowSignatures& next,
                           int prev_search_begin,
                           int prev_usable_end,
                           int next_min_template_start,
                           int next_usable_end) {
    OverlapResult fallback;
    if (prev.height <= 0 || next.height <= 0) return fallback;

    // Try several template start positions in `next`. The first one is the
    // caller's preferred start (usually top_bar + shared sticky). If that
    // fails — e.g., because the top of `next` is a carousel slide with no
    // counterpart in `prev` — we step further down by L rows and try again,
    // hoping to land in shared scroll content below any dynamic header area.
    //
    // We keep the best successful result (lowest mean-per-row cost), or, if
    // none succeed, the one with the lowest best_cost for diagnostics.
    const int content_height = next_usable_end - next_min_template_start;
    if (content_height <= 0) return fallback;

    const int approx_L = std::min(256, std::max(32, content_height / 3));
    const int step = std::max(approx_L, 128);

    // Candidate template starts: preferred, +step, +2*step, ...,
    // capped so we still have at least (approx_L / 2) rows of template.
    const int max_start = next_usable_end - approx_L / 2;

    OverlapResult best;
    best.best_cost = std::numeric_limits<double>::max();

    for (int ts = next_min_template_start; ts <= max_start; ts += step) {
        OverlapResult r = match_at(prev, next,
                                   prev_search_begin, prev_usable_end,
                                   ts, next_usable_end);
        if (r.ok) {
            // Accept the first successful template. Earlier template starts
            // are preferable because they minimize the amount of next-image
            // content we (implicitly) claim overlaps — a conservative choice
            // that avoids discarding real new scroll content.
            return r;
        }
        // Retain the lowest-cost attempt for diagnostic reporting.
        if (r.template_length > 0) {
            const double mean = r.best_cost / static_cast<double>(r.template_length);
            const double bmean = best.template_length > 0
                ? best.best_cost / static_cast<double>(best.template_length)
                : std::numeric_limits<double>::max();
            if (mean < bmean) best = r;
        }
    }
    return best;
}

void refine_overlap_seam(const RowSignatures& prev,
                         const RowSignatures& next,
                         OverlapResult& result,
                         int usable_end) {
    // Default: seam at the end of overlap = standard prefer-previous.
    result.seam_in_prev = usable_end;
    if (!result.ok) return;

    const int ov_begin = result.offset_in_prev;
    const int ov_end   = usable_end;
    const int ov_height = ov_end - ov_begin;
    if (ov_height < 20) return;   // overlap too small to bother

    // Per-row L1 above this ⇒ floating UI element in one of the images.
    constexpr int kDirtyL1 = 150;

    // Check the bottom few rows.  If they're clean, there's no dirty tail
    // and we keep the default seam.
    int dirty_in_bottom = 0;
    const int check = std::min(8, ov_height);
    for (int y = ov_end - 1; y >= ov_end - check; --y) {
        int ny = result.template_start_in_next + (y - ov_begin);
        if (row_l1(prev.row(y), next.row(ny)) > kDirtyL1)
            ++dirty_in_bottom;
    }
    if (dirty_in_bottom < check / 2) return;   // bottom is mostly clean

    // Bottom has a dirty tail (floating element in prev's lower rows).
    // Scan upward to find where the clean zone begins.  Require a run of
    // kMinClean consecutive clean rows to be robust against noise.
    constexpr int kMinClean = 3;
    int clean_run = 0;

    for (int y = ov_end - 1; y >= ov_begin; --y) {
        int ny = result.template_start_in_next + (y - ov_begin);
        int l1 = row_l1(prev.row(y), next.row(ny));

        if (l1 <= kDirtyL1) {
            ++clean_run;
            if (clean_run >= kMinClean) {
                // Place seam right after this stable clean zone.
                result.seam_in_prev = y + clean_run;
                return;
            }
        } else {
            clean_run = 0;
        }
    }
    // Entire overlap is dirty — keep default (don't break the output).
}

} // namespace picmerge
