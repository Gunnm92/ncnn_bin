#pragma once

#include <vector>
#include <cstdint>
#include <cmath>

/**
 * Tiling utilities for processing large images in chunks to reduce memory usage.
 *
 * Problem: Upscaling a 4K image (3840x2160) with 4x scale requires:
 * - Input RGB: ~24MB
 * - Output RGB: ~384MB (15360x8640)
 *
 * Solution: Split into tiles (e.g., 512x512), process separately, reassemble.
 * - Per tile input: ~0.75MB
 * - Per tile output: ~12MB (2048x2048)
 * - Memory reduction: 32x less peak memory!
 */

namespace tiling {

/// Configuration for tile-based processing
struct TilingConfig {
    int tile_size = 512;           // Base tile size (before upscaling)
    int overlap = 32;              // Overlap between tiles to avoid seams
    int scale_factor = 4;          // Upscale factor (2x, 3x, 4x)
    bool enable_tiling = true;     // Auto-enable for large images
    int threshold_width = 1000;    // Enable tiling if width > threshold (lowered to prevent OOM when Vulkan fails)
    int threshold_height = 1000;   // Enable tiling if height > threshold (lowered to prevent OOM when Vulkan fails)
};

/// Represents a single tile region
struct Tile {
    int x;           // Top-left X coordinate in source image
    int y;           // Top-left Y coordinate in source image
    int width;       // Tile width (may be smaller at edges)
    int height;      // Tile height (may be smaller at edges)
    int output_x;    // Target X in output image (after upscaling)
    int output_y;    // Target Y in output image (after upscaling)
};

/// Calculate tiles needed for an image
std::vector<Tile> calculate_tiles(
    int image_width,
    int image_height,
    const TilingConfig& config
);

/// Extract tile data from source image (RGB format)
bool extract_tile(
    const uint8_t* source_rgb,
    int source_width,
    int source_height,
    const Tile& tile,
    std::vector<uint8_t>& tile_data
);

/// Blend tile into output image with overlap smoothing (RGB format)
bool blend_tile(
    const uint8_t* tile_rgb,
    int tile_width,
    int tile_height,
    const Tile& tile,
    const TilingConfig& config,
    uint8_t* output_rgb,
    int output_width,
    int output_height
);

/// Check if tiling should be enabled for given dimensions
inline bool should_enable_tiling(
    int width,
    int height,
    const TilingConfig& config
) {
    return config.enable_tiling &&
           (width > config.threshold_width || height > config.threshold_height);
}

} // namespace tiling
