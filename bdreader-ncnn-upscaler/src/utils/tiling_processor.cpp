#include "tiling_processor.hpp"
#include "image_padding.hpp"
#include <algorithm>
#include <cstring>

namespace tiling {

bool process_with_tiling(
    BaseEngine* engine,
    const uint8_t* input_data,
    size_t input_size,
    std::vector<uint8_t>& output_data,
    const std::string& output_format
) {
    if (!engine) {
        logger::error("Tiling: null engine pointer");
        return false;
    }

    try {
        // Step 1: Decode compressed input to RGB
        image_io::ImagePixels source_image;
        if (!image_io::decode_image(input_data, input_size, source_image)) {
            logger::error("Tiling: failed to decode input image");
            return false;
        }

        // Step 2: Check if tiling is needed
        const tiling::TilingConfig config = engine->get_tiling_config();
        const bool needs_tiling = tiling::should_enable_tiling(
            source_image.width, source_image.height, config
        );

        if (!needs_tiling) {
            // Small image - process directly without tiling
            logger::info("Tiling: image too small (" + std::to_string(source_image.width) + "x" +
                        std::to_string(source_image.height) + " <= threshold " +
                        std::to_string(config.threshold_width) + "x" +
                        std::to_string(config.threshold_height) + "), processing directly");
            std::vector<uint8_t> output_rgb;
            int output_width, output_height;

            if (!engine->process_rgb(source_image.pixels.data(),
                                     source_image.width,
                                     source_image.height,
                                     output_rgb,
                                     output_width,
                                     output_height)) {
                logger::error("Tiling: direct processing failed");
                return false;
            }

            // Compress output
            image_io::ImagePixels output_pixels;
            output_pixels.width = output_width;
            output_pixels.height = output_height;
            output_pixels.channels = 3;
            output_pixels.pixels = std::move(output_rgb);

            if (!image_io::encode_image(output_pixels, output_format, output_data)) {
                logger::error("Tiling: failed to encode output");
                return false;
            }

            return true;
        }

        // Step 3: Calculate tiles
        const std::vector<Tile> tiles = tiling::calculate_tiles(
            source_image.width, source_image.height, config
        );

        if (tiles.empty()) {
            logger::error("Tiling: no tiles generated");
            return false;
        }

        // Step 4: Store original dimensions (before any padding from process_rgb)
        const int original_width = source_image.width;
        const int original_height = source_image.height;

        // Allocate output RGB buffer (may be larger due to padding in tiles)
        const int output_width = source_image.width * config.scale_factor;
        const int output_height = source_image.height * config.scale_factor;
        std::vector<uint8_t> output_rgb(output_width * output_height * 3, 0);

        logger::info("Tiling: processing " + std::to_string(tiles.size()) +
                     " tiles → output " + std::to_string(output_width) + "x" +
                     std::to_string(output_height));

        // Step 5: Process each tile (with per-tile exception handling)
        for (size_t i = 0; i < tiles.size(); ++i) {
            try {
                const Tile& tile = tiles[i];

                // Extract tile from source
                std::vector<uint8_t> tile_rgb;
                if (!tiling::extract_tile(source_image.pixels.data(),
                                           source_image.width,
                                           source_image.height,
                                           tile,
                                           tile_rgb)) {
                    logger::error("Tiling: failed to extract tile " + std::to_string(i));
                    return false;
                }

                // Process tile
                std::vector<uint8_t> upscaled_tile_rgb;
                int upscaled_width, upscaled_height;
                if (!engine->process_rgb(tile_rgb.data(),
                                         tile.width,
                                         tile.height,
                                         upscaled_tile_rgb,
                                         upscaled_width,
                                         upscaled_height)) {
                    logger::error("Tiling: failed to process tile " + std::to_string(i));
                    return false;
                }

                // STEP 1: Crop padding from upscaled tile
                // process_rgb() adds 18px padding which is now upscaled (18 * scale_factor on each side)
                const int expected_width = tile.width * config.scale_factor;
                const int expected_height = tile.height * config.scale_factor;
                const int pad_pixels = image_padding::kDefaultUpscalerPadding * config.scale_factor;

                std::vector<uint8_t> cropped_tile_rgb;
                int cropped_width = expected_width;
                int cropped_height = expected_height;

                if (upscaled_width > expected_width || upscaled_height > expected_height) {
                    // Crop padding from tile to get exact expected dimensions
                    cropped_tile_rgb.resize(cropped_width * cropped_height * 3);

                    const int max_offset_x = std::max(0, upscaled_width - cropped_width);
                    const int max_offset_y = std::max(0, upscaled_height - cropped_height);
                    const int start_x = std::min(pad_pixels, max_offset_x);
                    const int start_y = std::min(pad_pixels, max_offset_y);

                    for (int row = 0; row < cropped_height; ++row) {
                        const uint8_t* src_row = upscaled_tile_rgb.data() + ((start_y + row) * upscaled_width + start_x) * 3;
                        uint8_t* dst_row = cropped_tile_rgb.data() + row * cropped_width * 3;
                        std::memcpy(dst_row, src_row, cropped_width * 3);
                    }
                } else {
                    cropped_tile_rgb = std::move(upscaled_tile_rgb);
                }

                // STEP 2: Extract non-overlapping region (center cropping)
                // For non-border tiles, skip the overlap region at the start to avoid duplication
                int src_offset_x = 0;
                int src_offset_y = 0;
                int blend_width = cropped_width;
                int blend_height = cropped_height;

                // If not the first tile in X direction, skip the overlap at the start
                if (tile.output_x > 0) {
                    const int overlap_scaled = config.overlap * config.scale_factor;
                    src_offset_x = overlap_scaled;
                    blend_width -= overlap_scaled;
                }

                // If not the first tile in Y direction, skip the overlap at the start
                if (tile.output_y > 0) {
                    const int overlap_scaled = config.overlap * config.scale_factor;
                    src_offset_y = overlap_scaled;
                    blend_height -= overlap_scaled;
                }

                logger::info("Tiling: tile " + std::to_string(i) +
                            " cropped=" + std::to_string(cropped_width) + "x" + std::to_string(cropped_height) +
                            " blend_region=" + std::to_string(blend_width) + "x" + std::to_string(blend_height) +
                            " offset=(" + std::to_string(src_offset_x) + "," + std::to_string(src_offset_y) + ")");

                // Extract only the non-overlapping region to blend
                std::vector<uint8_t> region_to_blend;
                region_to_blend.resize(blend_width * blend_height * 3);

                for (int row = 0; row < blend_height; ++row) {
                    const uint8_t* src_row = cropped_tile_rgb.data() + ((src_offset_y + row) * cropped_width + src_offset_x) * 3;
                    uint8_t* dst_row = region_to_blend.data() + row * blend_width * 3;
                    std::memcpy(dst_row, src_row, blend_width * 3);
                }

                // Blend only the non-overlapping region into output
                if (!tiling::blend_tile(region_to_blend.data(),
                                        blend_width,
                                        blend_height,
                                        tile,
                                        config,
                                        output_rgb.data(),
                                        output_width,
                                        output_height)) {
                    logger::error("Tiling: failed to blend tile " + std::to_string(i));
                    return false;
                }

                // NOTE: Do NOT call cleanup() here - it corrupts the NCNN model.
                // Cleanup is handled by the caller at the end of the process/batch.

                // Progress logging every 10 tiles
                if ((i + 1) % 10 == 0 || (i + 1) == tiles.size()) {
                    logger::info("Tiling: processed " + std::to_string(i + 1) + "/" +
                                 std::to_string(tiles.size()) + " tiles");
                }
                
            } catch (const std::exception& e) {
                logger::error("Tiling: exception processing tile " + std::to_string(i) +
                             ": " + std::string(e.what()));
                return false;
            }
        }

        // Step 6: Crop padding from final output
        // Each tile was processed with padding (18px) which is now upscaled
        // We need to crop to original dimensions × scale_factor
        const int desired_width = original_width * config.scale_factor;
        const int desired_height = original_height * config.scale_factor;
        const bool needs_crop = (output_width > desired_width || output_height > desired_height);

        std::vector<uint8_t> final_pixels;
        int final_width = output_width;
        int final_height = output_height;

        if (needs_crop) {
            // Padding was added by process_rgb (18px * scale_factor)
            const int pad_pixels = image_padding::kDefaultUpscalerPadding * config.scale_factor;

            final_width = desired_width;
            final_height = desired_height;
            final_pixels.resize(final_width * final_height * 3);

            const int max_offset_x = std::max(0, output_width - final_width);
            const int max_offset_y = std::max(0, output_height - final_height);
            const int start_x = std::min(pad_pixels, max_offset_x);
            const int start_y = std::min(pad_pixels, max_offset_y);

            logger::info("Tiling: cropping from " + std::to_string(output_width) + "x" + std::to_string(output_height) +
                        " to " + std::to_string(final_width) + "x" + std::to_string(final_height) +
                        " (removing " + std::to_string(pad_pixels) + "px padding)");

            for (int row = 0; row < final_height; ++row) {
                const uint8_t* src_row = output_rgb.data() + ((start_y + row) * output_width + start_x) * 3;
                uint8_t* dst_row = final_pixels.data() + row * final_width * 3;
                std::memcpy(dst_row, src_row, final_width * 3);
            }
        } else {
            final_pixels = std::move(output_rgb);
        }

        // Step 7: Compress final output
        image_io::ImagePixels final_output;
        final_output.width = final_width;
        final_output.height = final_height;
        final_output.channels = 3;
        final_output.pixels = std::move(final_pixels);

        if (!image_io::encode_image(final_output, output_format, output_data)) {
            logger::error("Tiling: failed to encode final output");
            return false;
        }

        logger::info("Tiling: complete! Output size: " + std::to_string(output_data.size()) + " bytes");
        return true;
        
    } catch (const std::exception& e) {
        logger::error("Tiling: exception in process_with_tiling: " + std::string(e.what()));
        return false;
    } catch (...) {
        logger::error("Tiling: unknown exception in process_with_tiling");
        return false;
    }
}

} // namespace tiling
