// sticky_header.h
//
// Detect a secondary "sticky" header region that appears only once the user
// has scrolled past a certain point (e.g. the "搜索框 + 宝贝/评价/详情/推荐"
// tabs in the demo e-commerce screenshots).
//
// Definition used here: the largest S >= 0 such that rows
//     [top_bar, top_bar + S)
// of image (N+1) have pixel-identical signatures to the SAME rows of
// image N. A non-zero S means image N already contains this header at the
// same Y position, so image (N+1) must not emit it again.

#pragma once

#include "row_signature.h"

namespace picmerge {

// Returns the sticky header height (in rows) of `next` relative to `prev`.
// Returns 0 when no matching rows are found directly below the top bar.
//
// `max_sticky` caps the result (e.g. 40% of image height) as a safety bound.
int detect_sticky_header(const RowSignatures& prev,
                         const RowSignatures& next,
                         int top_bar_height,
                         int bottom_bar_height,
                         int max_sticky);

} // namespace picmerge
