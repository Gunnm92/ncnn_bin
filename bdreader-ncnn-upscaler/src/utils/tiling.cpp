#include "tiling.hpp"
#include "logger.hpp"
#include <algorithm>
#include <cstring>

namespace tiling {

std::vector<Tile> calculate_tiles(
    int image_width,
    int image_height,
    const TilingConfig& config
) {
    std::vector<Tile> tiles;

    const int tile_step = config.tile_size - config.overlap;

    // Calculate number of tiles needed
    int tiles_x = std::ceil(static_cast<float>(image_width - config.overlap) / tile_step);
    int tiles_y = std::ceil(static_cast<float>(image_height - config.overlap) / tile_step);

    logger::info("Tiling: image " + std::to_string(image_width) + "x" + std::to_string(image_height) +
                " → " + std::to_string(tiles_x) + "x" + std::to_string(tiles_y) + " tiles (size=" +
                std::to_string(config.tile_size) + ", overlap=" + std::to_string(config.overlap) + ")");

    for (int ty = 0; ty < tiles_y; ++ty) {
        for (int tx = 0; tx < tiles_x; ++tx) {
            Tile tile;

            // Source coordinates (before upscaling)
            tile.x = tx * tile_step;
            tile.y = ty * tile_step;
            tile.width = std::min(config.tile_size, image_width - tile.x);
            tile.height = std::min(config.tile_size, image_height - tile.y);

            // Output coordinates (after upscaling)
            // Note: Output position excludes overlap to avoid double-blending
            int effective_x = (tx == 0) ? 0 : tile.x + config.overlap;
            int effective_y = (ty == 0) ? 0 : tile.y + config.overlap;
            tile.output_x = effective_x * config.scale_factor;
            tile.output_y = effective_y * config.scale_factor;

            tiles.push_back(tile);
        }
    }

    logger::info("Tiling: generated " + std::to_string(tiles.size()) + " tiles");
    return tiles;
}

bool extract_tile(
    const uint8_t* source_rgb,
    int source_width,
    int source_height,
    const Tile& tile,
    std::vector<uint8_t>& tile_data
) {
    if (!source_rgb) {
        logger::error("Tiling: extract_tile() null source pointer");
        return false;
    }

    // Allocate tile buffer (RGB = 3 channels)
    const size_t tile_size = tile.width * tile.height * 3;
    tile_data.resize(tile_size);

    // Copy rows from source to tile
    for (int y = 0; y < tile.height; ++y) {
        const int source_y = tile.y + y;
        if (source_y >= source_height) break;

        const uint8_t* source_row = source_rgb + (source_y * source_width + tile.x) * 3;
        uint8_t* tile_row = tile_data.data() + (y * tile.width) * 3;

        const int copy_width = std::min(tile.width, source_width - tile.x);
        std::memcpy(tile_row, source_row, copy_width * 3);
    }

    return true;
}

bool blend_tile(
    const uint8_t* tile_rgb,
    int tile_width,
    int tile_height,
    const Tile& tile,
    const TilingConfig& config,
    uint8_t* output_rgb,
    int output_width,
    int output_height
) {
    if (!tile_rgb || !output_rgb) {
        logger::error("Tiling: blend_tile() null pointer");
        return false;
    }

    // Note: tile.output_x and tile.output_y ALREADY exclude the overlap
    // for non-border tiles (see calculate_tiles). So we copy the ENTIRE
    // upscaled tile to avoid gaps.

    // Copy entire tile to output with bounds checking
    for (int y = 0; y < tile_height; ++y) {
        const int output_y = tile.output_y + y;
        if (output_y >= output_height) break;

        for (int x = 0; x < tile_width; ++x) {
            const int output_x = tile.output_x + x;
            if (output_x >= output_width) break;

            const int tile_offset = (y * tile_width + x) * 3;
            const int output_offset = (output_y * output_width + output_x) * 3;

            // Direct copy - overlap is handled by output position calculation
            output_rgb[output_offset + 0] = tile_rgb[tile_offset + 0]; // R
            output_rgb[output_offset + 1] = tile_rgb[tile_offset + 1]; // G
            output_rgb[output_offset + 2] = tile_rgb[tile_offset + 2]; // B
        }
    }

    return true;
}

} // namespace tiling
