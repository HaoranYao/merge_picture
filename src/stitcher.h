// stitcher.h
//
// Takes the planning results (fixed bars, per-image sticky header heights,
// per-pair overlap offsets) and produces the final long image.

#pragma once

#include <string>
#include <vector>

#include "overlap_finder.h"
#include "row_signature.h"

namespace picmerge {

struct Contribution {
    int image_index;
    int y_begin;
    int y_end;
};

struct StitchPlan {
    int width = 0;
    int height = 0;
    int top_bar = 0;
    int bottom_bar = 0;
    std::vector<Contribution> parts;
};

StitchPlan plan_stitch(int width,
                       int image_height,
                       int num_images,
                       int top_bar,
                       int bottom_bar,
                       int bar_ref_image,
                       const std::vector<int>& self_sticky,
                       const std::vector<int>& fallback_skip,
                       const std::vector<OverlapResult>& overlaps);

bool execute_stitch(const StitchPlan& plan,
                    const std::vector<std::string>& input_paths,
                    const std::string& output_path,
                    int jpeg_quality);

bool execute_stitch_from_raw_cache(const StitchPlan& plan,
                                   const std::vector<std::string>& raw_cache_paths,
                                   const std::string& output_path,
                                   int jpeg_quality);

} // namespace picmerge
