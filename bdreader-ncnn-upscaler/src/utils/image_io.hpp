#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace image_io {

struct ImagePixels {
    int width = 0;
    int height = 0;
    int channels = 0;
    std::vector<uint8_t> pixels;
};

bool decode_image(const uint8_t* data, size_t size, ImagePixels& out);
bool encode_image(const ImagePixels& img, const std::string& format, std::vector<uint8_t>& out);

} // namespace image_io
