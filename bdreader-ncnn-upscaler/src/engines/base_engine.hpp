#pragma once

#include "../options.hpp"
#include "../utils/tiling.hpp"
#include <cstdint>
#include <string>
#include <vector>

struct ImageBuffer {
    std::vector<uint8_t> data;
    int width = 0;
    int height = 0;
    int channels = 0;
};

class BaseEngine {
public:
    virtual ~BaseEngine() = default;
    virtual bool init(const Options& opts) = 0;

    /// Process single image (wrapper that auto-selects tiling or direct processing)
    virtual bool process_single(const uint8_t* input_data, size_t input_size,
        std::vector<uint8_t>& output_data, const std::string& output_format) = 0;

    /// Process RGB buffer directly (used internally by tiling)
    /// Returns RGB buffer (not compressed)
    virtual bool process_rgb(const uint8_t* rgb_data, int width, int height,
        std::vector<uint8_t>& output_rgb, int& output_width, int& output_height) = 0;

    virtual bool process_batch(const std::vector<ImageBuffer>& inputs,
        std::vector<ImageBuffer>& outputs, const std::string& output_format) = 0;
    virtual void cleanup() = 0;

    /// Get tiling configuration (can be overridden per engine)
    virtual tiling::TilingConfig get_tiling_config() const {
        tiling::TilingConfig config;
        config.tile_size = 512;
        config.overlap = 32;
        config.scale_factor = get_scale_factor();
        config.threshold_width = 2048;
        config.threshold_height = 2048;
        return config;
    }

    /// Get upscale factor (must be implemented by subclasses)
    virtual int get_scale_factor() const = 0;
};
