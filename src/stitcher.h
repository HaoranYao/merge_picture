// stitcher.h
//
// Takes the planning results (fixed bars, per-image sticky header heights,
// per-pair overlap offsets) and produces the final long image.
//
// Memory policy:
//   - Exactly one pre-allocated output buffer (W × H_out × 3 bytes).
//   - At any moment, at most ONE decoded input image is held in memory.
//   - Each input image is loaded, its contribution rows memcpy'd into the
//     output buffer, then freed immediately.

#pragma once

#include <string>
#include <vector>

#include "overlap_finder.h"
#include "row_signature.h"

namespace picmerge {

// One contiguous row span that an image contributes to the final output.
struct Contribution {
    int image_index;   // index into input_paths
    int y_begin;       // inclusive, source-image coordinates
    int y_end;         // exclusive
};

struct StitchPlan {
    int width  = 0;
    int height = 0;     // total output height
    int top_bar = 0;
    int bottom_bar = 0;
    std::vector<Contribution> parts;  // in output-row order
};

// Pure planning: given metadata, decide who contributes what rows.
// `self_sticky[i]` is the sticky-header height of image i in its own
// coordinate system (already merged from the pairwise detection).
// `overlaps[i]` is the overlap result for pair (i, i+1); only i in
// [0, N-2] is populated.
StitchPlan plan_stitch(int width,
                       int image_height,
                       int num_images,
                       int top_bar,
                       int bottom_bar,
                       const std::vector<int>& self_sticky,
                       const std::vector<OverlapResult>& overlaps);

// Execute a plan: load each image in turn, memcpy its contribution rows,
// encode the output as JPEG. Returns false on any I/O or alloc failure.
bool execute_stitch(const StitchPlan& plan,
                    const std::vector<std::string>& input_paths,
                    const std::string& output_path,
                    int jpeg_quality);

} // namespace picmerge
