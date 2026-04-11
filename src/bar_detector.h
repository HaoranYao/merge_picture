// bar_detector.h
//
// Detect the fixed top (OS status bar) and bottom (app navigation bar)
// regions by finding the longest prefix / suffix of rows whose signatures
// are identical across ALL input images.
//
// These bars must only appear once in the final merged image (PRD §3).

#pragma once

#include <vector>

#include "row_signature.h"

namespace picmerge {

struct FixedBars {
    int top_height    = 0;  // rows [0, top_height)           are the shared top bar
    int bottom_height = 0;  // rows [H - bottom_height, H)    are the shared bottom bar
};

// `sigs` must be non-empty, and every element must have the same `.height`.
// Safety cap: top_height and bottom_height are each clamped to at most
// `max_fraction` of the image height (default 20%) to prevent runaway
// detection when two images happen to share long constant stretches.
FixedBars detect_fixed_bars(const std::vector<RowSignatures>& sigs,
                            double max_fraction = 0.20);

} // namespace picmerge
