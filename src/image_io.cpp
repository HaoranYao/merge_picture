// image_io.cpp

#include "image_io.h"

// stb lives in exactly one translation unit.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <utility>

namespace picmerge {

bool probe_image(const std::string& path, int& width, int& height) {
    int channels = 0;
    return stbi_info(path.c_str(), &width, &height, &channels) != 0;
}

Image::~Image() {
    reset();
}

Image::Image(Image&& other) noexcept
    : data_(other.data_), width_(other.width_), height_(other.height_) {
    other.data_ = nullptr;
    other.width_ = 0;
    other.height_ = 0;
}

Image& Image::operator=(Image&& other) noexcept {
    if (this != &other) {
        reset();
        data_   = other.data_;
        width_  = other.width_;
        height_ = other.height_;
        other.data_ = nullptr;
        other.width_ = 0;
        other.height_ = 0;
    }
    return *this;
}

bool Image::load(const std::string& path) {
    reset();
    int channels_in_file = 0;
    data_ = stbi_load(path.c_str(), &width_, &height_, &channels_in_file, kChannels);
    if (!data_) {
        width_ = height_ = 0;
        return false;
    }
    return true;
}

void Image::reset() {
    if (data_) {
        stbi_image_free(data_);
        data_ = nullptr;
    }
    width_ = 0;
    height_ = 0;
}

bool write_jpeg(const std::string& path,
                int width,
                int height,
                const uint8_t* rgb,
                int quality) {
    if (quality < 1)   quality = 1;
    if (quality > 100) quality = 100;
    return stbi_write_jpg(path.c_str(), width, height, kChannels, rgb, quality) != 0;
}

} // namespace picmerge
