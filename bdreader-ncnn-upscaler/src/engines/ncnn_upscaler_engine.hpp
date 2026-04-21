#pragma once

#include "base_engine.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "../options.hpp"
#include "../utils/image_io.hpp"
#include "../utils/logger.hpp"
#include "allocator.h"
#include "net.h"

/// Shared NCNN-backed upscaler engine. Implements the full pipeline
/// (init, model load, preprocessing, inference dispatch, cropping, cleanup)
/// so concrete engines only provide model selection and extractor wiring.
class NcnnUpscalerEngine : public BaseEngine {
public:
    // Destructor is defaulted on purpose: cleanup() calls virtual hooks (engine_name()),
    // which would resolve to the pure-virtual base during ~NcnnUpscalerEngine. Derived
    // classes must invoke cleanup() from their own destructor while their vtable is still
    // the most-derived one.
    ~NcnnUpscalerEngine() override = default;

    bool init(const Options& opts) override;
    bool process_single(const uint8_t* input_data, size_t input_size,
        std::vector<uint8_t>& output_data, const std::string& output_format) override;
    bool process_batch(const std::vector<ImageBuffer>& inputs,
        std::vector<ImageBuffer>& outputs, const std::string& output_format) override;
    bool process_rgb(const uint8_t* rgb_data, int width, int height,
        std::vector<uint8_t>& output_rgb, int& output_width, int& output_height) override;
    void cleanup() override;
    void clear_allocators() override;
    tiling::TilingConfig get_tiling_config() const override;

protected:
    // ---- Hooks for concrete engines ----

    /// Short engine name used in log messages (e.g. "RealCUGAN").
    virtual const char* engine_name() const = 0;

    /// Path used when opts.model is empty. Return "" to require an explicit path.
    virtual std::filesystem::path default_model_root() const = 0;

    /// Pick the model basename (without .param/.bin) from current_options_.
    virtual std::string choose_model() const = 0;

    /// Fallback basename if the chosen one is missing on disk.
    virtual std::string fallback_model_name() const = 0;

    /// Engine-specific NCNN extractor wiring. Return true on success.
    virtual bool run_inference_impl(const ncnn::Mat& input, ncnn::Mat& output) = 0;

    /// Optional hook: adjust current_options_ right after init() copies opts in.
    virtual void on_options_loaded() {}

    // ---- Shared helpers used by both engines (not part of BaseEngine API) ----

    bool process_image(const image_io::ImagePixels& decoded, image_io::ImagePixels& encoded);
    bool run_inference(const ncnn::Mat& input, ncnn::Mat& output);
    bool run_inference(const ncnn::Mat& input, ncnn::Mat& output, bool allow_fallback);

    bool load_model();
    void ensure_cpu_mode();
    void apply_cpu_low_mem_profile();
    void apply_igpu_profile(int device_id);
    void setup_cpu_allocators();
    void clear_cpu_allocators();
#if NCNN_VULKAN
    void setup_vulkan_allocators(int device_id);
    void release_vulkan_allocators();
#endif

    // ---- Shared state ----

    Options current_options_{};
    ncnn::Net net_;
    std::optional<std::filesystem::path> model_root_;
    bool use_vulkan_ = true;
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
