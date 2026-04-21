#include "ncnn_upscaler_engine.hpp"

#include "../utils/image_padding.hpp"
#include "../utils/tiling_processor.hpp"

#include <algorithm>
#include <cstring>
#include "net.h"
#if NCNN_VULKAN
#include "gpu.h"
#endif

bool NcnnUpscalerEngine::init(const Options& opts) {
    current_options_ = opts;
    on_options_loaded();

    std::filesystem::path candidate(opts.model);
    if (candidate.empty()) {
        candidate = default_model_root();
    }
    if (candidate.empty()) {
        logger::error(std::string(engine_name()) + " model path is empty");
        return false;
    }
    if (!std::filesystem::exists(candidate)) {
        logger::warn(std::string(engine_name()) + " model directory not found: " + candidate.string());
    }
    model_root_ = candidate;

    use_vulkan_ = true;
    net_.opt.use_vulkan_compute = true;
    net_.opt.use_fp16_storage = true;
    net_.opt.use_fp16_arithmetic = true;
    net_.opt.use_fp16_packed = true;

    // Memory-saving optimizations for batch processing (v4 pipeline)
    // Reference: https://github.com/Tencent/ncnn/wiki/use-ncnn-with-alexnet#vulkan
    net_.opt.lightmode = true;
    net_.opt.use_winograd_convolution = false;

    int device_id = 0;
    if (opts.gpu_id == "auto" || opts.gpu_id.empty()) {
        device_id = 0;
    } else {
        try {
            device_id = std::stoi(opts.gpu_id);
        } catch (...) {
            device_id = 0;
        }
    }
    if (device_id >= 0) {
#if NCNN_VULKAN
        net_.set_vulkan_device(device_id);
        setup_vulkan_allocators(device_id);
        apply_igpu_profile(device_id);
#else
        logger::warn("Vulkan support was disabled at build time, running CPU mode");
        ensure_cpu_mode();
        use_vulkan_ = false;
#endif
    } else {
        ensure_cpu_mode();
        use_vulkan_ = false;
    }

    if (!use_vulkan_) {
        setup_cpu_allocators();
        apply_cpu_low_mem_profile();
    }

    return load_model();
}

bool NcnnUpscalerEngine::load_model() {
    if (!model_root_) {
        logger::error(std::string("No ") + engine_name() + " model root configured");
        return false;
    }

    const std::string base = choose_model();
    auto build_param = [&](const std::string& name) {
        return model_root_.value() / (name + ".param");
    };
    auto build_bin = [&](const std::string& name) {
        return model_root_.value() / (name + ".bin");
    };

    std::filesystem::path param = build_param(base);
    std::filesystem::path bin = build_bin(base);

    if (!std::filesystem::exists(param) || !std::filesystem::exists(bin)) {
        const std::string fallback = fallback_model_name();
        logger::warn(std::string("Specified ") + engine_name() +
                     " model missing, falling back to " + fallback);
        param = build_param(fallback);
        bin = build_bin(fallback);
    }

    if (!std::filesystem::exists(param) || !std::filesystem::exists(bin)) {
        logger::error(std::string(engine_name()) + " fallback model missing: " + param.string());
        return false;
    }

    if (net_.load_param(param.string().c_str()) != 0) {
        logger::error(std::string("Failed to load ") + engine_name() + " param: " + param.string());
        return false;
    }
    if (net_.load_model(bin.string().c_str()) != 0) {
        logger::error(std::string("Failed to load ") + engine_name() + " bin: " + bin.string());
        return false;
    }

    logger::info(std::string("Loaded ") + engine_name() + " model: " + param.filename().string());
    return true;
}

bool NcnnUpscalerEngine::run_inference(const ncnn::Mat& input, ncnn::Mat& output) {
    return run_inference(input, output, true);
}

bool NcnnUpscalerEngine::run_inference(const ncnn::Mat& input, ncnn::Mat& output, bool allow_fallback) {
    if (run_inference_impl(input, output)) {
        return true;
    }

    if (allow_fallback && use_vulkan_) {
        logger::warn(std::string(engine_name()) + " Vulkan inference failed; falling back to CPU.");
#if NCNN_VULKAN
        release_vulkan_allocators();
#endif
        ensure_cpu_mode();
        use_vulkan_ = false;
        setup_cpu_allocators();
        apply_cpu_low_mem_profile();
        return run_inference_impl(input, output);
    }

    return false;
}

bool NcnnUpscalerEngine::process_image(const image_io::ImagePixels& decoded,
    image_io::ImagePixels& encoded) {
    ncnn::Mat in;
    ncnn::Mat result;

    try {
        const image_io::ImagePixels padded_input = image_padding::pad_image(decoded);
        in = ncnn::Mat::from_pixels(padded_input.pixels.data(), ncnn::Mat::PIXEL_RGB,
            padded_input.width, padded_input.height);

        const float norm_vals[3] = {1.0f / 255.0f, 1.0f / 255.0f, 1.0f / 255.0f};
        in.substract_mean_normalize(0, norm_vals);

        if (!run_inference(in, result)) {
            logger::error(std::string(engine_name()) + " process_image: inference failed");
            throw std::runtime_error("Inference failed");
        }

        // Denormalize float [0, 1] → [0, 255]. to_pixels() clamps to uint8 safely.
        for (int c = 0; c < 3; ++c) {
            float* channel_ptr = result.channel(c);
            const int channel_size = result.w * result.h;
            for (int i = 0; i < channel_size; ++i) {
                channel_ptr[i] *= 255.0f;
            }
        }

        std::vector<uint8_t> full_pixels(result.w * result.h * 3);
        result.to_pixels(full_pixels.data(), ncnn::Mat::PIXEL_RGB);

        const int scale = get_scale_factor();
        const int desired_width = decoded.width * scale;
        const int desired_height = decoded.height * scale;
        const int full_width = result.w;
        const int full_height = result.h;
        const bool needs_crop = full_width > desired_width || full_height > desired_height;

        std::vector<uint8_t> final_pixels;
        int final_width = full_width;
        int final_height = full_height;

        if (needs_crop) {
            final_width = desired_width;
            final_height = desired_height;
            final_pixels.resize(final_width * final_height * 3);
            const int pad_pixels = image_padding::kDefaultUpscalerPadding * scale;
            const int max_offset_x = std::max(0, full_width - final_width);
            const int max_offset_y = std::max(0, full_height - final_height);
            const int start_x = std::min(pad_pixels, max_offset_x);
            const int start_y = std::min(pad_pixels, max_offset_y);

            for (int row = 0; row < final_height; ++row) {
                const uint8_t* src_row = full_pixels.data() + ((start_y + row) * full_width + start_x) * 3;
                uint8_t* dst_row = final_pixels.data() + row * final_width * 3;
                std::memcpy(dst_row, src_row, final_width * 3);
            }
        } else {
            final_pixels = std::move(full_pixels);
        }

        encoded.width = final_width;
        encoded.height = final_height;
        encoded.channels = 3;
        encoded.pixels = std::move(final_pixels);

        result.release();
        in.release();
        clear_cpu_allocators();
        return true;

    } catch (const std::exception& e) {
        logger::error(std::string(engine_name()) + " process_image exception: " + e.what());
        if (result.data) result.release();
        if (in.data) in.release();
        clear_cpu_allocators();
        return false;
    } catch (...) {
        logger::error(std::string(engine_name()) + " process_image unknown exception");
        if (result.data) result.release();
        if (in.data) in.release();
        clear_cpu_allocators();
        return false;
    }
}

bool NcnnUpscalerEngine::process_single(const uint8_t* input_data, size_t input_size,
    std::vector<uint8_t>& output_data, const std::string& output_format) {
    return tiling::process_with_tiling(this, input_data, input_size,
                                       output_data, output_format);
}

bool NcnnUpscalerEngine::process_batch(const std::vector<ImageBuffer>& inputs,
    std::vector<ImageBuffer>& outputs, const std::string& output_format) {
    outputs.clear();
    outputs.reserve(inputs.size());

    for (const auto& input : inputs) {
        ImageBuffer result{};
        std::vector<uint8_t> compressed;
        if (!process_single(input.data.data(), input.data.size(), compressed, output_format)) {
            logger::warn(std::string(engine_name()) + " batch: inference failed");
            outputs.push_back(result);
            continue;
        }
        result.data = std::move(compressed);
        outputs.push_back(std::move(result));
    }

    return true;
}

bool NcnnUpscalerEngine::process_rgb(const uint8_t* rgb_data, int width, int height,
    std::vector<uint8_t>& output_rgb, int& output_width, int& output_height) {
    image_io::ImagePixels input;
    input.width = width;
    input.height = height;
    input.channels = 3;
    input.pixels.assign(rgb_data, rgb_data + (width * height * 3));

    image_io::ImagePixels output;
    if (!process_image(input, output)) {
        logger::error(std::string(engine_name()) + " process_rgb: inference failed");
        return false;
    }

    output_width = output.width;
    output_height = output.height;
    output_rgb = std::move(output.pixels);
    return true;
}

void NcnnUpscalerEngine::clear_allocators() {
#if NCNN_VULKAN
    if (use_vulkan_) {
        if (blob_vkallocator_)    blob_vkallocator_->clear();
        if (staging_vkallocator_) staging_vkallocator_->clear();
    } else {
#endif
        cpu_blob_allocator_.clear();
        cpu_workspace_allocator_.clear();
#if NCNN_VULKAN
    }
#endif
}

void NcnnUpscalerEngine::cleanup() {
    // Idempotent: model_root_ is reset below and acts as the "already cleaned" flag.
    if (!model_root_.has_value()) {
        return;
    }

    logger::info(std::string(engine_name()) + " engine cleanup");

#if NCNN_VULKAN
    if (use_vulkan_) {
        net_.opt.use_vulkan_compute = false;
    }
    release_vulkan_allocators();
#endif

    net_.clear();

    use_vulkan_ = false;
    model_root_.reset();
    clear_cpu_allocators();

    logger::info(std::string(engine_name()) + " engine cleanup complete");
}

tiling::TilingConfig NcnnUpscalerEngine::get_tiling_config() const {
    tiling::TilingConfig config = BaseEngine::get_tiling_config();
    if (current_options_.tile_size > 0) {
        config.tile_size = std::max(config.overlap + 1, current_options_.tile_size);
        config.threshold_width = std::max(1, config.tile_size);
        config.threshold_height = std::max(1, config.tile_size);
    } else if (igpu_profile_) {
        config.tile_size = std::max(config.overlap + 1, 384);
        config.threshold_width = std::min(config.threshold_width, 1024);
        config.threshold_height = std::min(config.threshold_height, 1024);
    }
    return config;
}

void NcnnUpscalerEngine::ensure_cpu_mode() {
    net_.opt.use_vulkan_compute = false;
    net_.opt.use_fp16_storage = false;
    net_.opt.use_fp16_arithmetic = false;
    net_.opt.use_fp16_packed = false;
}

void NcnnUpscalerEngine::apply_cpu_low_mem_profile() {
    if (cpu_low_mem_) {
        return;
    }
    cpu_low_mem_ = true;
    net_.opt.num_threads = std::min(4, std::max(1, net_.opt.num_threads));
    net_.opt.openmp_blocktime = 0;
    net_.opt.use_winograd_convolution = false;
    net_.opt.use_sgemm_convolution = false;
    net_.opt.use_packing_layout = false;
    net_.opt.use_local_pool_allocator = true;
    logger::info(std::string(engine_name()) + " CPU low-mem profile enabled");
}

void NcnnUpscalerEngine::apply_igpu_profile(int device_id) {
#if NCNN_VULKAN
    if (igpu_profile_ || device_id < 0) {
        return;
    }
    const ncnn::GpuInfo& info = ncnn::get_gpu_info(device_id);
    const bool is_integrated = (info.type() == 1) || (info.vendor_id() == 0x8086);
    if (!is_integrated) {
        return;
    }
    igpu_profile_ = true;
    net_.opt.use_winograd_convolution = false;
    net_.opt.use_sgemm_convolution = false;
    net_.opt.use_packing_layout = false;
    net_.opt.use_cooperative_matrix = false;
    net_.opt.use_fp16_storage = info.support_fp16_storage();
    net_.opt.use_fp16_arithmetic = info.support_fp16_arithmetic();
    net_.opt.use_fp16_packed = info.support_fp16_packed();
    logger::info(std::string(engine_name()) + " iGPU profile enabled (" + info.device_name() + ")");
#else
    (void)device_id;
#endif
}

void NcnnUpscalerEngine::setup_cpu_allocators() {
    net_.opt.blob_allocator = &cpu_blob_allocator_;
    net_.opt.workspace_allocator = &cpu_workspace_allocator_;
}

void NcnnUpscalerEngine::clear_cpu_allocators() {
    if (!use_vulkan_) {
        cpu_blob_allocator_.clear();
        cpu_workspace_allocator_.clear();
    }
}

#if NCNN_VULKAN
void NcnnUpscalerEngine::setup_vulkan_allocators(int device_id) {
    if (device_id < 0 || blob_vkallocator_ || staging_vkallocator_) {
        return;
    }
    vkdev_ = ncnn::get_gpu_device(device_id);
    if (!vkdev_) {
        return;
    }
    blob_vkallocator_ = vkdev_->acquire_blob_allocator();
    staging_vkallocator_ = vkdev_->acquire_staging_allocator();
    net_.opt.blob_vkallocator = blob_vkallocator_;
    net_.opt.workspace_vkallocator = blob_vkallocator_;
    net_.opt.staging_vkallocator = staging_vkallocator_;
}

void NcnnUpscalerEngine::release_vulkan_allocators() {
    net_.opt.blob_vkallocator = nullptr;
    net_.opt.workspace_vkallocator = nullptr;
    net_.opt.staging_vkallocator = nullptr;
    if (vkdev_ && blob_vkallocator_) {
        vkdev_->reclaim_blob_allocator(blob_vkallocator_);
    }
    if (vkdev_ && staging_vkallocator_) {
        vkdev_->reclaim_staging_allocator(staging_vkallocator_);
    }
    blob_vkallocator_ = nullptr;
    staging_vkallocator_ = nullptr;
    vkdev_ = nullptr;
}
#endif
