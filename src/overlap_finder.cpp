// overlap_finder.cpp
//
// 1D sliding-window search using L1 distance on per-row fingerprints.
// Never touches the 2D pixel buffer: this is the core PRD performance
// constraint.

#include "overlap_finder.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace picmerge {

namespace {

constexpr int kTemplateCap = 256;
constexpr int kCoarseFactor = 4;
constexpr int kFineWindow = 24;
constexpr double kOverlapOkMean = 100.0;
constexpr double kEarlyStopMean = 30.0;
constexpr double kFullSearchFallbackMean = 60.0;

struct SearchResult {
    int64_t best_cost = std::numeric_limits<int64_t>::max();
    int64_t second_cost = std::numeric_limits<int64_t>::max();
    int best_d = -1;
};

RowSignatures downsample_signatures(const RowSignatures& src, int factor) {
    RowSignatures out;
    if (src.height <= 0 || factor <= 1) return src;

    out.height = (src.height + factor - 1) / factor;
    out.fp.assign(static_cast<size_t>(out.height) * kSigBins, 0);

    for (int y = 0; y < out.height; ++y) {
        const int begin = y * factor;
        const int end = std::min(src.height, begin + factor);
        const int count = end - begin;
        uint8_t* dst = out.fp.data() + static_cast<size_t>(y) * kSigBins;
        for (int bin = 0; bin < kSigBins; ++bin) {
            unsigned sum = 0;
            for (int sy = begin; sy < end; ++sy) sum += src.row(sy)[bin];
            dst[bin] = static_cast<uint8_t>(sum / static_cast<unsigned>(count));
        }
    }
    return out;
}

SearchResult search_range(const RowSignatures& prev,
                          const RowSignatures& next,
                          int search_begin,
                          int search_end,
                          int template_start,
                          int template_length) {
    SearchResult result;
    if (template_length <= 0 || search_end < search_begin) return result;

    for (int d = search_begin; d <= search_end; ++d) {
        int64_t cost = 0;
        bool pruned = false;
        for (int k = 0; k < template_length; ++k) {
            cost += row_l1(prev.row(d + k), next.row(template_start + k));
            if (cost >= result.best_cost) {
                pruned = true;
                break;
            }
        }
        if (pruned) continue;

        if (cost < result.best_cost) {
            if (result.best_d < 0 || std::abs(d - result.best_d) > template_length / 2) {
                result.second_cost = result.best_cost;
            }
            result.best_cost = cost;
            result.best_d = d;
        } else if (cost < result.second_cost &&
                   (result.best_d < 0 || std::abs(d - result.best_d) > template_length / 2)) {
            result.second_cost = cost;
        }
    }

    return result;
}

OverlapResult match_at(const RowSignatures& prev,
                       const RowSignatures& next,
                       const RowSignatures* prev_coarse,
                       const RowSignatures* next_coarse,
                       int prev_search_begin,
                       int prev_usable_end,
                       int next_template_start,
                       int next_usable_end) {
    OverlapResult r;
    r.template_start_in_next = next_template_start;

    const int next_content_height = next_usable_end - next_template_start;
    if (next_content_height <= 0) return r;

    int template_length = std::min(kTemplateCap, next_content_height / 3);
    if (template_length < 32) template_length = std::min(next_content_height, 32);
    if (template_length <= 0) return r;
    r.template_length = template_length;

    const int search_begin = prev_search_begin;
    const int search_end = prev_usable_end - template_length;
    if (search_end < search_begin) return r;

    SearchResult fine;
    if (prev_coarse && next_coarse) {
        const int coarse_template_start = next_template_start / kCoarseFactor;
        const int coarse_template_end =
            (next_template_start + template_length + kCoarseFactor - 1) / kCoarseFactor;
        const int coarse_length = coarse_template_end - coarse_template_start;
        const int coarse_search_begin = search_begin / kCoarseFactor;
        const int coarse_search_end =
            std::min(prev_coarse->height - coarse_length, search_end / kCoarseFactor);

        if (coarse_length > 0 && coarse_search_end >= coarse_search_begin) {
            const SearchResult coarse = search_range(*prev_coarse, *next_coarse,
                                                     coarse_search_begin, coarse_search_end,
                                                     coarse_template_start, coarse_length);
            if (coarse.best_d >= 0) {
                const int center = coarse.best_d * kCoarseFactor;
                const int fine_slack = kFineWindow + (kCoarseFactor - 1);
                fine = search_range(prev, next,
                                    std::max(search_begin, center - fine_slack),
                                    std::min(search_end, center + fine_slack),
                                    next_template_start, template_length);
            }
        }
    }

    if (fine.best_d < 0) {
        fine = search_range(prev, next,
                            search_begin, search_end,
                            next_template_start, template_length);
    } else {
        const double local_mean =
            static_cast<double>(fine.best_cost) / static_cast<double>(template_length);
        if (local_mean > kFullSearchFallbackMean) {
            fine = search_range(prev, next,
                                search_begin, search_end,
                                next_template_start, template_length);
        }
    }

    r.best_cost = static_cast<double>(fine.best_cost);
    r.second_best_cost = static_cast<double>(fine.second_cost);
    if (fine.best_d < 0) return r;

    const double mean_per_row =
        static_cast<double>(fine.best_cost) / static_cast<double>(template_length);
    if (mean_per_row < kOverlapOkMean) {
        r.ok = true;
        r.offset_in_prev = fine.best_d;
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

    const int content_height = next_usable_end - next_min_template_start;
    if (content_height <= 0) return fallback;

    const int approx_template = std::min(kTemplateCap, std::max(32, content_height / 3));
    const int step = std::max(approx_template, 128);
    const int max_start = next_usable_end - approx_template / 2;

    const RowSignatures prev_coarse = downsample_signatures(prev, kCoarseFactor);
    const RowSignatures next_coarse = downsample_signatures(next, kCoarseFactor);

    OverlapResult best;
    best.best_cost = std::numeric_limits<double>::max();

    for (int ts = next_min_template_start; ts <= max_start; ts += step) {
        OverlapResult r = match_at(prev, next,
                                   &prev_coarse, &next_coarse,
                                   prev_search_begin, prev_usable_end,
                                   ts, next_usable_end);
        const double mean = r.template_length > 0
            ? r.best_cost / static_cast<double>(r.template_length)
            : std::numeric_limits<double>::max();
        if (r.ok || mean < kEarlyStopMean) {
            return r;
        }
        if (r.template_length > 0) {
            const double best_mean = best.template_length > 0
                ? best.best_cost / static_cast<double>(best.template_length)
                : std::numeric_limits<double>::max();
            if (mean < best_mean) best = r;
        }
    }
    return best;
}

void refine_overlap_seam(const RowSignatures& prev,
                         const RowSignatures& next,
                         OverlapResult& result,
                         int usable_end) {
    result.seam_in_prev = usable_end;
    if (!result.ok) return;

    const int ov_begin = result.offset_in_prev;
    const int ov_end = usable_end;
    const int ov_height = ov_end - ov_begin;
    if (ov_height < 20) return;

    constexpr int kDirtyL1 = 150;

    int dirty_in_bottom = 0;
    const int check = std::min(8, ov_height);
    for (int y = ov_end - 1; y >= ov_end - check; --y) {
        const int ny = result.template_start_in_next + (y - ov_begin);
        if (row_l1(prev.row(y), next.row(ny)) > kDirtyL1)
            ++dirty_in_bottom;
    }
    if (dirty_in_bottom < check / 2) return;

    constexpr int kMinClean = 3;
    constexpr int kSeamMargin = 10;
    int clean_run = 0;

    for (int y = ov_end - 1; y >= ov_begin; --y) {
        const int ny = result.template_start_in_next + (y - ov_begin);
        const int l1 = row_l1(prev.row(y), next.row(ny));

        if (l1 <= kDirtyL1) {
            ++clean_run;
            if (clean_run >= kMinClean) {
                int seam = y + clean_run;
                seam = std::max(ov_begin, seam - kSeamMargin);
                result.seam_in_prev = seam;
                return;
            }
        } else {
            clean_run = 0;
        }
    }
}

} // namespace picmerge
