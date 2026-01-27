#pragma once

#include "../engines/base_engine.hpp"
#include "tiling.hpp"
#include "image_io.hpp"
#include "logger.hpp"
#include <vector>

namespace tiling {

/**
 * Process image with automatic tiling
 *
 * This function:
 * 1. Decodes compressed input (JPEG/PNG/WebP)
 * 2. Checks if tiling is needed (based on dimensions)
 * 3. If yes: splits into tiles, processes each, reassembles
 * 4. If no: processes directly
 * 5. Compresses final result to output format
 *
 * Memory optimization:
 * - Only 1 tile in memory at a time (~12MB vs 384MB for full image)
 * - Source RGB kept in memory (needed for tile extraction)
 * - Output RGB accumulated progressively
 *
 * @param engine Engine to use for processing (RealCUGAN, RealESRGAN)
 * @param input_data Compressed input image bytes
 * @param input_size Size of input_data
 * @param output_data Output compressed image bytes (will be resized)
 * @param output_format Output format ("webp", "png", "jpg")
 * @return true on success, false on error
 */
bool process_with_tiling(
    BaseEngine* engine,
    const uint8_t* input_data,
    size_t input_size,
    std::vector<uint8_t>& output_data,
    const std::string& output_format
);

} // namespace tiling
