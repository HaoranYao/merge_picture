// row_signature.cpp

#include "row_signature.h"

#include <cstdint>

namespace picmerge {

namespace {

inline unsigned fast_divide(unsigned sum, unsigned divisor, uint64_t recip) {
    unsigned q = static_cast<unsigned>((static_cast<uint64_t>(sum) * recip) >> 32);
    if (static_cast<uint64_t>(q) * divisor > sum) {
        --q;
    } else if (static_cast<uint64_t>(q + 1) * divisor <= sum) {
        ++q;
    }
    return q;
}

inline unsigned sum_bytes(const uint8_t* p, int count) {
    unsigned sum = 0;
    int i = 0;

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    for (; i + 16 <= count; i += 16) {
        sum += static_cast<unsigned>(detail::horizontal_sum_u8(vld1q_u8(p + i)));
    }
#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
    const __m128i zero = _mm_setzero_si128();
    for (; i + 16 <= count; i += 16) {
        const __m128i bytes = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i));
        sum += static_cast<unsigned>(detail::sad_sum(_mm_sad_epu8(bytes, zero)));
    }
#endif

    for (; i < count; ++i) sum += p[i];
    return sum;
}

} // namespace

RowSignatures compute_row_signatures(const Image& img) {
    RowSignatures out;
    const int H = img.height();
    const int W = img.width();
    out.height = H;
    if (H <= 0 || W <= 0 || !img.data()) return out;

    // Central 50% of the row width. Excludes edge floating widgets
    // (e.g. right-side 红包雨 badge, left-side side menu).
    const int x_begin = W / 4;
    const int x_end   = W - W / 4;
    const int slice_pixels = x_end - x_begin;
    if (slice_pixels < kSigBins) return out;   // absurdly narrow input

    const int bin_pixels = slice_pixels / kSigBins;
    const int bin_bytes  = bin_pixels * kChannels;
    const unsigned bin_divisor = static_cast<unsigned>(bin_bytes);
    const uint64_t bin_recip = (((uint64_t)1 << 32) + bin_divisor - 1) / bin_divisor;

    const size_t row_stride = static_cast<size_t>(W) * kChannels;
    const size_t col_offset = static_cast<size_t>(x_begin) * kChannels;

    out.fp.assign(static_cast<size_t>(H) * kSigBins, 0);

    const uint8_t* base = img.data();
    for (int y = 0; y < H; ++y) {
        const uint8_t* rowp = base + static_cast<size_t>(y) * row_stride + col_offset;
        uint8_t* fp = out.fp.data() + static_cast<size_t>(y) * kSigBins;

        for (int b = 0; b < kSigBins; ++b) {
            const uint8_t* p = rowp + static_cast<size_t>(b) * bin_bytes;
            const unsigned sum = sum_bytes(p, bin_bytes);
            fp[b] = static_cast<uint8_t>(fast_divide(sum, bin_divisor, bin_recip));
        }
    }
    return out;
}

} // namespace picmerge
