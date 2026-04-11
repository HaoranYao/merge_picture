// row_signature.h
//
// Per-row fingerprint used as the 1D representation of an image for all
// downstream alignment work. This is the key optimization that satisfies
// PRD's "no brute-force full-resolution pixel matching" constraint: after
// this step, we never touch the 2D pixel array for matching again — only
// the small 1D fingerprint array.
//
// Each row is reduced to `kSigBins = 16` bytes. We take the central 50% of
// the row (excluding edge floating widgets like the 红包雨 badge), split it
// into 16 equal pixel bins, and store the mean of all bytes in each bin
// (across R/G/B). This gives a lossy-but-noise-tolerant fingerprint: two
// rows whose content differs only due to JPEG recompression map to very
// similar (not identical) fingerprints, and we match with L1 distance.
//
// Memory: 16 × H bytes per image (~42 KB for a 2622-tall screenshot).

#pragma once

#include <cstdint>
#include <vector>

#include "image_io.h"

namespace picmerge {

constexpr int kSigBins = 16;

struct RowSignatures {
    // Flat array; fingerprint of row y starts at &fp[y * kSigBins].
    std::vector<uint8_t> fp;
    int height = 0;

    const uint8_t* row(int y) const {
        return fp.data() + static_cast<size_t>(y) * kSigBins;
    }
};

// Compute per-row fingerprints for `img`. Single linear scan.
RowSignatures compute_row_signatures(const Image& img);

// Returns true if both rows differ by at most `tol` on every one of the
// 16 fingerprint bytes. Used for bar/sticky detection.
inline bool rows_match(const uint8_t* a, const uint8_t* b, int tol) {
    for (int k = 0; k < kSigBins; ++k) {
        int d = static_cast<int>(a[k]) - static_cast<int>(b[k]);
        if (d < 0) d = -d;
        if (d > tol) return false;
    }
    return true;
}

// Sum of absolute differences between two fingerprints (range [0, 16*255]).
// Used as the per-row cost during overlap search.
inline int row_l1(const uint8_t* a, const uint8_t* b) {
    int s = 0;
    for (int k = 0; k < kSigBins; ++k) {
        int d = static_cast<int>(a[k]) - static_cast<int>(b[k]);
        s += (d < 0) ? -d : d;
    }
    return s;
}

} // namespace picmerge
