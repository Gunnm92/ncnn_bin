#pragma once

#include <algorithm>

#include "image_io.hpp"

namespace image_padding {

constexpr int kDefaultUpscalerPadding = 18;

inline image_io::ImagePixels pad_image(const image_io::ImagePixels& src, int padding = kDefaultUpscalerPadding) {
    if (src.width <= 0 || src.height <= 0 || padding <= 0) {
        return src;
    }

    image_io::ImagePixels padded;
    padded.channels = src.channels;
    padded.width = src.width + padding * 2;
    padded.height = src.height + padding * 2;
    padded.pixels.resize(padded.width * padded.height * padded.channels);

    const int max_x = src.width > 0 ? src.width - 1 : 0;
    const int max_y = src.height > 0 ? src.height - 1 : 0;

    for (int y = 0; y < padded.height; ++y) {
        const int src_y = std::clamp(y - padding, 0, max_y);
        for (int x = 0; x < padded.width; ++x) {
            const int src_x = std::clamp(x - padding, 0, max_x);
            const int dst_index = (y * padded.width + x) * padded.channels;
            const int src_index = (src_y * src.width + src_x) * src.channels;
            for (int c = 0; c < padded.channels; ++c) {
                padded.pixels[dst_index + c] = src.pixels[src_index + c];
            }
        }
    }

    return padded;
}

} // namespace image_padding
