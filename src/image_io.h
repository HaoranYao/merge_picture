// image_io.h
//
// RAII wrapper around stb_image for loading 8-bit RGB images and encoding
// JPEGs. Kept intentionally small: we only need to load a whole screenshot,
// walk its rows, then free it again.

#pragma once

#include <cstdint>
#include <string>

namespace picmerge {

// Always decode to 3-channel (RGB). stb_image will drop alpha if present,
// which simplifies every downstream row stride calculation.
constexpr int kChannels = 3;

// Quick metadata probe without decoding pixel data. Returns true on success.
bool probe_image(const std::string& path, int& width, int& height);

// Owns a decoded image buffer allocated by stb_image. RAII: frees on destroy.
// Non-copyable, movable.
class Image {
public:
    Image() = default;
    ~Image();

    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    Image(Image&& other) noexcept;
    Image& operator=(Image&& other) noexcept;

    // Decode the file at `path` into memory as 8-bit RGB. Returns false on
    // failure; on success width()/height()/data() become valid.
    bool load(const std::string& path);

    // Release the underlying buffer immediately (equivalent to destruction).
    void reset();

    int width()  const { return width_; }
    int height() const { return height_; }

    // Pointer to the first byte of the image (row-major, RGBRGB... per row).
    const uint8_t* data() const { return data_; }

    // Pointer to the first byte of row `y`.
    const uint8_t* row(int y) const {
        return data_ + static_cast<size_t>(y) * static_cast<size_t>(width_) * kChannels;
    }

private:
    uint8_t* data_ = nullptr;   // owned by stb_image, freed via stbi_image_free
    int width_ = 0;
    int height_ = 0;
};

// Encode an RGB buffer as a JPEG. `quality` is in [1, 100].
bool write_jpeg(const std::string& path,
                int width,
                int height,
                const uint8_t* rgb,
                int quality = 90);

} // namespace picmerge
