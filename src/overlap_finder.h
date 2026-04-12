// overlap_finder.h
//
// Given row signatures for two adjacent screenshots, find the vertical scroll
// offset at which the second image's content is already present in the first.
//
// This operates purely on the 1D signature array: we never revisit the full
// pixel buffer to perform a 2D search. That is the core PRD performance
// constraint.

#pragma once

#include "row_signature.h"

namespace picmerge {

struct OverlapResult {
    bool  ok = false;                    // false => caller should fall back
    int   offset_in_prev = 0;            // row in `prev` where `next`'s template starts
    int   template_start_in_next = 0;    // = top_bar + max(self_sticky[N], self_sticky[N+1])
    int   template_length = 0;
    int   seam_in_prev = 0;              // optimal seam within overlap (default = usable_end)
    // Diagnostics
    double best_cost = 0.0;
    double second_best_cost = 0.0;
};

// 1D sliding-window search using L1 distance on per-row fingerprints.
//
//   prev_search_begin          inclusive lower bound for `offset_in_prev`
//   prev_usable_end            exclusive upper bound for `offset_in_prev + L`
//   next_min_template_start    earliest Y in `next` the template may start at
//   next_usable_end            exclusive upper bound for template rows in `next`
//
// Internally tries multiple template start positions within `next` starting
// at `next_min_template_start` and stepping down by ~L rows. This makes the
// matcher robust to the "hero carousel swap" failure mode where the top of
// one screenshot contains dynamic content (different carousel slide, themed
// status bar) that has no counterpart at the same Y in the other image,
// but shared scroll content still exists further down.
//
// On `ok = false` the caller should fall back to a direct concatenation
// (no overlap skipped).
OverlapResult find_overlap(const RowSignatures& prev,
                           const RowSignatures& next,
                           int prev_search_begin,
                           int prev_usable_end,
                           int next_min_template_start,
                           int next_usable_end);

// Refine the seam position within an already-detected overlap.
//
// By default, the stitcher uses the previous image's pixels for the entire
// overlap region (prefer-previous).  This works well unless there is a
// floating UI element (promo banner, "快要抢光" badge, etc.) near the
// bottom of the previous image — those pixels are "dirty".  The same page
// content appears near the top of the next image, where no floating element
// covers it.
//
// This function scans the bottom of the overlap.  If the two images disagree
// there (high per-row L1 — a sign of a floating overlay in one of them), it
// moves `result.seam_in_prev` upward so that the dirty tail is provided by
// the next image instead.  When the bottom is clean, the seam stays at
// `usable_end` and the behaviour is identical to the original code.
void refine_overlap_seam(const RowSignatures& prev,
                         const RowSignatures& next,
                         OverlapResult& result,
                         int usable_end);

} // namespace picmerge
