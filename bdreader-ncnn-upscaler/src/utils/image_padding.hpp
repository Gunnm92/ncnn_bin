#pragma once

#include <algorithm>
#include <cstring>

#include "image_io.hpp"

namespace image_padding {

constexpr int kDefaultUpscalerPadding = 18;

inline image_io::ImagePixels pad_image(const image_io::ImagePixels& src, int padding = kDefaultUpscalerPadding) {
    if (src.width <= 0 || src.height <= 0 || padding <= 0) {
        return src;
    }

    const int ch = src.channels;
    image_io::ImagePixels padded;
    padded.channels = ch;
    // Round padded dimensions up to the nearest even number.
    // NCNN CPU inference produces incorrect output (near-blank image) for odd dimensions,
    // regardless of the low-mem profile settings. Aligning to 2 prevents this.
    padded.width  = (src.width  + padding * 2 + 1) & ~1;
    padded.height = (src.height + padding * 2 + 1) & ~1;
    padded.pixels.resize(padded.width * padded.height * ch);

    const int max_y = src.height - 1;
    const int src_row_bytes = src.width * ch;
    const int dst_row_bytes = padded.width * ch;

    for (int y = 0; y < padded.height; ++y) {
        const int src_y = std::clamp(y - padding, 0, max_y);
        uint8_t* dst_row = padded.pixels.data() + y * dst_row_bytes;
        const uint8_t* src_row = src.pixels.data() + src_y * src_row_bytes;

        // Left padding: replicate leftmost source pixel
        const uint8_t* left_px = src_row;
        for (int x = 0; x < padding; ++x) {
            std::memcpy(dst_row + x * ch, left_px, ch);
        }

        // Center: copy the source row directly
        std::memcpy(dst_row + padding * ch, src_row, src_row_bytes);

        // Right padding: replicate rightmost source pixel
        const uint8_t* right_px = src_row + (src.width - 1) * ch;
        for (int x = 0; x < padding; ++x) {
            std::memcpy(dst_row + (padding + src.width + x) * ch, right_px, ch);
        }
    }

    return padded;
}

} // namespace image_padding
