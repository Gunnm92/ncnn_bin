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
    // Guard against images smaller than the overlap to avoid negative division
    int tiles_x = std::max(1, static_cast<int>(std::ceil(static_cast<float>(std::max(0, image_width - config.overlap)) / tile_step)));
    int tiles_y = std::max(1, static_cast<int>(std::ceil(static_cast<float>(std::max(0, image_height - config.overlap)) / tile_step)));

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
    const int copy_width = std::max(0, std::min(tile_width, output_width - tile.output_x));
    const int max_rows = std::max(0, std::min(tile_height, output_height - tile.output_y));
    if (copy_width == 0 || max_rows == 0) {
        return true;
    }

    const size_t row_bytes = static_cast<size_t>(copy_width) * 3;
    for (int y = 0; y < max_rows; ++y) {
        const uint8_t* src_row = tile_rgb + static_cast<size_t>(y) * tile_width * 3;
        uint8_t* dst_row = output_rgb + (static_cast<size_t>(tile.output_y + y) * output_width + tile.output_x) * 3;
        std::memcpy(dst_row, src_row, row_bytes);
    }

    return true;
}

} // namespace tiling
