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

#include <cstddef>
#include <cstdint>
#include <vector>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#endif

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

namespace detail {

#if defined(__ARM_NEON) || defined(__ARM_NEON__)

inline int horizontal_sum_u8(uint8x16_t values) {
#if defined(__aarch64__) || defined(_M_ARM64)
    return static_cast<int>(vaddlvq_u8(values));
#else
    const uint16x8_t sum16 = vpaddlq_u8(values);
    const uint32x4_t sum32 = vpaddlq_u16(sum16);
    const uint64x2_t sum64 = vpaddlq_u32(sum32);
    return static_cast<int>(vgetq_lane_u64(sum64, 0) + vgetq_lane_u64(sum64, 1));
#endif
}

inline int row_l1_simd(const uint8_t* a, const uint8_t* b) {
    return horizontal_sum_u8(vabdq_u8(vld1q_u8(a), vld1q_u8(b)));
}

inline int row_edge_l1_simd(const uint8_t* a, const uint8_t* b) {
    static const uint8_t kMaskBytes[16] = {
        0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff
    };
    const uint8x16_t mask = vld1q_u8(kMaskBytes);
    const uint8x16_t va = vandq_u8(vld1q_u8(a), mask);
    const uint8x16_t vb = vandq_u8(vld1q_u8(b), mask);
    return horizontal_sum_u8(vabdq_u8(va, vb));
}

#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)

inline int sad_sum(__m128i sad) {
    alignas(16) unsigned long long lanes[2];
    _mm_storeu_si128(reinterpret_cast<__m128i*>(lanes), sad);
    return static_cast<int>(lanes[0] + lanes[1]);
}

inline int row_l1_simd(const uint8_t* a, const uint8_t* b) {
    const __m128i va = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a));
    const __m128i vb = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b));
    return sad_sum(_mm_sad_epu8(va, vb));
}

inline int row_edge_l1_simd(const uint8_t* a, const uint8_t* b) {
    const __m128i mask = _mm_setr_epi8(
        static_cast<char>(0xff), static_cast<char>(0xff),
        static_cast<char>(0xff), static_cast<char>(0xff),
        0, 0, 0, 0, 0, 0, 0, 0,
        static_cast<char>(0xff), static_cast<char>(0xff),
        static_cast<char>(0xff), static_cast<char>(0xff));
    const __m128i va = _mm_and_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a)), mask);
    const __m128i vb = _mm_and_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(b)), mask);
    return sad_sum(_mm_sad_epu8(va, vb));
}

#endif

} // namespace detail

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
#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
    return detail::row_l1_simd(a, b);
#else
    int s = 0;
    for (int k = 0; k < kSigBins; ++k) {
        int d = static_cast<int>(a[k]) - static_cast<int>(b[k]);
        s += (d < 0) ? -d : d;
    }
    return s;
#endif
}

// L1 on the outer bins only. Used by bottom-bar detection to tolerate
// dynamic center text while still matching stable left/right chrome.
inline int row_edge_l1(const uint8_t* a, const uint8_t* b) {
#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
    return detail::row_edge_l1_simd(a, b);
#else
    int s = 0;
    for (int k = 0; k < 4; ++k) {
        int d = static_cast<int>(a[k]) - static_cast<int>(b[k]);
        s += (d < 0) ? -d : d;
    }
    for (int k = kSigBins - 4; k < kSigBins; ++k) {
        int d = static_cast<int>(a[k]) - static_cast<int>(b[k]);
        s += (d < 0) ? -d : d;
    }
    return s;
#endif
}

} // namespace picmerge
