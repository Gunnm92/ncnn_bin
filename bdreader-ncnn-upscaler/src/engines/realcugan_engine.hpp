#pragma once

#include "base_engine.hpp"
#include <cstdint>
#include <filesystem>
#include <optional>
#include "../utils/image_io.hpp"
#include "../utils/logger.hpp"
#include "allocator.h"
#include "net.h"

class RealCUGANEngine : public BaseEngine {
public:
    RealCUGANEngine() = default;
    bool init(const Options& opts) override;
    bool process_single(const uint8_t* input_data, size_t input_size,
        std::vector<uint8_t>& output_data, const std::string& output_format) override;
    bool process_batch(const std::vector<ImageBuffer>& inputs,
        std::vector<ImageBuffer>& outputs, const std::string& output_format) override;
    void cleanup() override;
    tiling::TilingConfig get_tiling_config() const override;

    /// Process RGB buffer directly (for tiling support)
    bool process_rgb(const uint8_t* rgb_data, int width, int height,
        std::vector<uint8_t>& output_rgb, int& output_width, int& output_height) override;

    /// Get upscale factor
    int get_scale_factor() const override;

private:
    Options current_options_;
    ncnn::Net net_;
    std::optional<std::filesystem::path> model_root_;
    bool use_vulkan_ = true;

    bool load_model();
    bool run_inference(const ncnn::Mat& input, ncnn::Mat& output);
    bool run_inference(const ncnn::Mat& input, ncnn::Mat& output, bool allow_fallback);
    bool process_image(const image_io::ImagePixels& decoded, image_io::ImagePixels& encoded);
    std::string choose_model() const;
    void ensure_cpu_mode();
    void apply_cpu_low_mem_profile();
    void apply_igpu_profile(int device_id);
    void setup_cpu_allocators();
    void clear_cpu_allocators();
#if NCNN_VULKAN
    void setup_vulkan_allocators(int device_id);
    void release_vulkan_allocators();
#endif
    bool cpu_low_mem_ = false;
    bool igpu_profile_ = false;

    ncnn::UnlockedPoolAllocator cpu_blob_allocator_;
    ncnn::PoolAllocator cpu_workspace_allocator_;
#if NCNN_VULKAN
    ncnn::VulkanDevice* vkdev_ = nullptr;
    ncnn::VkAllocator* blob_vkallocator_ = nullptr;
    ncnn::VkAllocator* staging_vkallocator_ = nullptr;
#endif
};
