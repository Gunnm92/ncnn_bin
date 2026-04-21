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

        // Step 4: Allocate output RGB buffer at final dimensions
        // Each tile's upscaled result will be cropped of its padding before being blended in.
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

                // Process tile — engines return the final upscaled tile at
                // (tile.width * scale) × (tile.height * scale), padding already cropped.
                std::vector<uint8_t> upscaled_tile_rgb;
                int upscaled_width = 0;
                int upscaled_height = 0;
                if (!engine->process_rgb(tile_rgb.data(),
                                         tile.width,
                                         tile.height,
                                         upscaled_tile_rgb,
                                         upscaled_width,
                                         upscaled_height)) {
                    logger::error("Tiling: failed to process tile " + std::to_string(i));
                    return false;
                }

                // Extract the non-overlapping region of this tile. For non-border tiles,
                // skip the overlap at the top/left to avoid duplicating pixels already
                // contributed by previous tiles.
                const int overlap_scaled = config.overlap * config.scale_factor;
                const int src_offset_x = (tile.output_x > 0) ? overlap_scaled : 0;
                const int src_offset_y = (tile.output_y > 0) ? overlap_scaled : 0;
                const int blend_width = upscaled_width - src_offset_x;
                const int blend_height = upscaled_height - src_offset_y;

                if (blend_width <= 0 || blend_height <= 0) {
                    continue;
                }

                std::vector<uint8_t> region_to_blend(blend_width * blend_height * 3);
                for (int row = 0; row < blend_height; ++row) {
                    const uint8_t* src_row = upscaled_tile_rgb.data() + ((src_offset_y + row) * upscaled_width + src_offset_x) * 3;
                    uint8_t* dst_row = region_to_blend.data() + row * blend_width * 3;
                    std::memcpy(dst_row, src_row, blend_width * 3);
                }

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

        // Step 6: Encode final output
        // output_rgb is already sized to original_width * scale × original_height * scale,
        // per-tile cropping has already removed the padding from each tile's contribution.
        image_io::ImagePixels final_output;
        final_output.width = output_width;
        final_output.height = output_height;
        final_output.channels = 3;
        final_output.pixels = std::move(output_rgb);

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
