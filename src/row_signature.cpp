// row_signature.cpp

#include "row_signature.h"

namespace picmerge {

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

    const size_t row_stride = static_cast<size_t>(W) * kChannels;
    const size_t col_offset = static_cast<size_t>(x_begin) * kChannels;

    out.fp.assign(static_cast<size_t>(H) * kSigBins, 0);

    const uint8_t* base = img.data();
    for (int y = 0; y < H; ++y) {
        const uint8_t* rowp = base + static_cast<size_t>(y) * row_stride + col_offset;
        uint8_t* fp = out.fp.data() + static_cast<size_t>(y) * kSigBins;

        for (int b = 0; b < kSigBins; ++b) {
            const uint8_t* p = rowp + static_cast<size_t>(b) * bin_bytes;
            unsigned sum = 0;
            for (int k = 0; k < bin_bytes; ++k) sum += p[k];
            fp[b] = static_cast<uint8_t>(sum / static_cast<unsigned>(bin_bytes));
        }
    }
    return out;
}

} // namespace picmerge
