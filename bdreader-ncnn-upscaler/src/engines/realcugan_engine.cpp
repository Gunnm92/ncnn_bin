#include "realcugan_engine.hpp"

#include "../utils/image_padding.hpp"
#include "../utils/tiling_processor.hpp"

#include <algorithm>
#include <cctype>
#include "net.h"
#if NCNN_VULKAN
#include "gpu.h"
#endif

namespace {
int quality_to_noise(const std::string& quality) {
    if (quality.empty()) {
        return -1;
    }
    const char flag = static_cast<char>(std::toupper(quality.front()));
    switch (flag) {
        case 'F': return -1;
        case 'E': return 0;
        case 'Q': return 1;
        case 'H': return 2;
        default: return -1;
    }
}

} // namespace

bool RealCUGANEngine::init(const Options& opts) {
    current_options_ = opts;
    current_options_.noise_level = opts.noise_level >= 0
        ? opts.noise_level
        : quality_to_noise(opts.quality);

    std::filesystem::path candidate(opts.model);
    if (candidate.empty()) {
        logger::error("RealCUGAN model path is empty");
        return false;
    }

    if (!std::filesystem::exists(candidate)) {
        logger::warn("RealCUGAN model directory not found: " + candidate.string());
    }
    model_root_ = candidate;

    use_vulkan_ = true;
    net_.opt.use_vulkan_compute = true;
    net_.opt.use_fp16_storage = true;
    net_.opt.use_fp16_arithmetic = true;
    net_.opt.use_fp16_packed = true;

    // Memory-saving optimizations for batch processing (v4 pipeline)
    // Reference: https://github.com/Tencent/ncnn/wiki/use-ncnn-with-alexnet#vulkan
    net_.opt.lightmode = true;                      // Reduce intermediate memory (~30% saving)
    net_.opt.use_winograd_convolution = false;      // Disable workspace allocation
    // Note: use_fp16_storage=true already set above (divides weight memory by 2)

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

bool RealCUGANEngine::load_model() {
    if (!model_root_) {
        logger::error("No RealCUGAN model root configured");
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
        logger::warn("Specified RealCUGAN model missing, falling back to up2x-conservative");
        param = build_param("up2x-conservative");
        bin = build_bin("up2x-conservative");
    }

    if (!std::filesystem::exists(param) || !std::filesystem::exists(bin)) {
        logger::error("RealCUGAN fallback model missing: " + param.string());
        return false;
    }

    if (net_.load_param(param.string().c_str()) != 0) {
        logger::error("Failed to load RealCUGAN param: " + param.string());
        return false;
    }
    if (net_.load_model(bin.string().c_str()) != 0) {
        logger::error("Failed to load RealCUGAN bin: " + bin.string());
        return false;
    }

    logger::info("Loaded RealCUGAN model: " + param.filename().string());
    return true;
}

std::string RealCUGANEngine::choose_model() const {
    switch (current_options_.noise_level) {
        case -1:
            return "up2x-no-denoise";
        case 0:
            return "up2x-denoise1x";
        case 1:
            return "up2x-denoise2x";
        case 2:
            return "up2x-denoise3x";
        case 3:
            return "up2x-denoise3x";
        default:
            return "up2x-conservative";
    }
}

bool RealCUGANEngine::run_inference(const ncnn::Mat& input, ncnn::Mat& output) {
    return run_inference(input, output, true);
}

bool RealCUGANEngine::run_inference(const ncnn::Mat& input, ncnn::Mat& output, bool allow_fallback) {
    ncnn::Extractor ex = net_.create_extractor();
    // Note: noise_level is baked into the model (up2x-denoise1x, up2x-denoise2x, etc.)
    // and is NOT a dynamic input parameter for these pre-compiled models
    ex.input("in0", input);
    const int ret = ex.extract("out0", output);
    if (ret == 0) {
        return true;
    }

    logger::warn("RealCUGAN inference returned " + std::to_string(ret));

    if (allow_fallback && use_vulkan_) {
        logger::warn("RealCUGAN Vulkan inference failed; falling back to CPU.");
#if NCNN_VULKAN
        release_vulkan_allocators();
#endif
        ensure_cpu_mode();
        use_vulkan_ = false;
        setup_cpu_allocators();
        apply_cpu_low_mem_profile();
        return run_inference(input, output, false);
    }

    return false;
}

bool RealCUGANEngine::process_image(const image_io::ImagePixels& decoded,
    image_io::ImagePixels& encoded) {
    // RAII scope guard for NCNN Mat cleanup
    // Ensures release() is called even if exception occurs
    ncnn::Mat in;
    ncnn::Mat result;

    try {
        // RealCUGAN expects RGB input, normalize to [0, 1]
        const image_io::ImagePixels padded_input = image_padding::pad_image(decoded);
        in = ncnn::Mat::from_pixels(padded_input.pixels.data(), ncnn::Mat::PIXEL_RGB,
            padded_input.width, padded_input.height);

        // Normalize: convert uint8 [0, 255] to float [0, 1]
        const float norm_vals[3] = {1.0f / 255.0f, 1.0f / 255.0f, 1.0f / 255.0f};
        in.substract_mean_normalize(0, norm_vals);

        if (!run_inference(in, result)) {
            logger::error("RealCUGAN process_image: inference failed");
            // Cleanup will happen in catch block
            throw std::runtime_error("Inference failed");
        }

        encoded.width = result.w;
        encoded.height = result.h;
        encoded.channels = 3;
        encoded.pixels.resize(encoded.width * encoded.height * encoded.channels);

        // Denormalize: RealCUGAN outputs float [0, 1], multiply by 255 to get [0, 255]
        const float denorm_vals[3] = {255.0f, 255.0f, 255.0f};
        const float mean_vals[3] = {0.0f, 0.0f, 0.0f};

        // Multiply each channel by 255
        for (int c = 0; c < 3; ++c) {
            float* channel_ptr = result.channel(c);
            const int channel_size = result.w * result.h;
            for (int i = 0; i < channel_size; ++i) {
                channel_ptr[i] *= 255.0f;
            }
        }

        // Now to_pixels will convert float [0, 255] to uint8
        result.to_pixels(encoded.pixels.data(), ncnn::Mat::PIXEL_RGB);

        // Explicitly release NCNN Mat GPU resources (success path)
        result.release();
        in.release();

        clear_cpu_allocators();
        return true;

    } catch (const std::exception& e) {
        logger::error("RealCUGAN process_image exception: " + std::string(e.what()));
        // Explicit cleanup in exception path to ensure GPU memory is freed
        if (result.data) result.release();
        if (in.data) in.release();
        clear_cpu_allocators();
        return false;
    } catch (...) {
        logger::error("RealCUGAN process_image unknown exception");
        // Explicit cleanup in exception path
        if (result.data) result.release();
        if (in.data) in.release();
        clear_cpu_allocators();
        return false;
    }
}

bool RealCUGANEngine::process_single(const uint8_t* input_data, size_t input_size,
    std::vector<uint8_t>& output_data, const std::string& output_format) {
    // Use tiling processor automatically (activates for images > 2048×2048)
    // This prevents OOM on large manga panels while maintaining performance on small images
    return tiling::process_with_tiling(this, input_data, input_size,
                                       output_data, output_format);
}

bool RealCUGANEngine::process_batch(const std::vector<ImageBuffer>& inputs,
    std::vector<ImageBuffer>& outputs, const std::string& output_format) {
    outputs.clear();
    outputs.reserve(inputs.size());

    for (const auto& input : inputs) {
        ImageBuffer result{};

        // Utilise le même chemin que process_single (tiling + reconstruction).
        std::vector<uint8_t> compressed;
        if (!process_single(input.data.data(), input.data.size(), compressed, output_format)) {
            logger::warn("RealCUGAN batch: inference failed");
            outputs.push_back(result);
            continue;
        }

        // Récupère largeur/hauteur via un décodage léger du résultat.
        image_io::ImagePixels decoded_out;
        if (image_io::decode_image(compressed.data(), compressed.size(), decoded_out)) {
            result.width = decoded_out.width;
            result.height = decoded_out.height;
            result.channels = decoded_out.channels;
        }

        result.data = std::move(compressed);
        outputs.push_back(std::move(result));
    }

    return true;
}

void RealCUGANEngine::cleanup() {
    // Guard against double cleanup (idempotent cleanup pattern)
    // If model_root_ is empty, cleanup has already been called
    if (!model_root_.has_value()) {
        logger::info("RealCUGAN engine already cleaned up, skipping");
        return;
    }

    logger::info("RealCUGAN engine cleanup");

#if NCNN_VULKAN
    // Explicitly release Vulkan resources BEFORE clearing network
    // This ensures proper cleanup order: Vulkan → NCNN → State
    if (use_vulkan_) {
        // Force release of any remaining Vulkan command buffers and descriptors
        // by resetting Vulkan compute flag (will be re-enabled in init() if needed)
        net_.opt.use_vulkan_compute = false;
    }
    release_vulkan_allocators();
#endif

    // Clear NCNN network (releases model weights and intermediate buffers)
    // Note: This should be called AFTER disabling Vulkan compute
    net_.clear();

    // Reset state flags
    use_vulkan_ = false;
    model_root_.reset();  // This acts as our "cleaned up" flag
    clear_cpu_allocators();

    logger::info("RealCUGAN engine cleanup complete");
}

bool RealCUGANEngine::process_rgb(const uint8_t* rgb_data, int width, int height,
    std::vector<uint8_t>& output_rgb, int& output_width, int& output_height) {
    // Convert raw RGB buffer to ImagePixels format
    image_io::ImagePixels input;
    input.width = width;
    input.height = height;
    input.channels = 3;
    input.pixels.assign(rgb_data, rgb_data + (width * height * 3));

    // Process using existing process_image logic
    image_io::ImagePixels output;
    if (!process_image(input, output)) {
        logger::error("RealCUGAN process_rgb: inference failed");
        return false;
    }

    // Return raw RGB buffer (not compressed)
    output_width = output.width;
    output_height = output.height;
    output_rgb = std::move(output.pixels);
    return true;
}

int RealCUGANEngine::get_scale_factor() const {
    return current_options_.scale;
}

tiling::TilingConfig RealCUGANEngine::get_tiling_config() const {
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

void RealCUGANEngine::ensure_cpu_mode() {
    net_.opt.use_vulkan_compute = false;
    net_.opt.use_fp16_storage = false;
    net_.opt.use_fp16_arithmetic = false;
    net_.opt.use_fp16_packed = false;
}

void RealCUGANEngine::apply_cpu_low_mem_profile() {
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
    logger::info("RealCUGAN CPU low-mem profile enabled");
}

void RealCUGANEngine::apply_igpu_profile(int device_id) {
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
    logger::info(std::string("RealCUGAN iGPU profile enabled (") + info.device_name() + ")");
#else
    (void)device_id;
#endif
}

void RealCUGANEngine::setup_cpu_allocators() {
    net_.opt.blob_allocator = &cpu_blob_allocator_;
    net_.opt.workspace_allocator = &cpu_workspace_allocator_;
}

void RealCUGANEngine::clear_cpu_allocators() {
    if (!use_vulkan_) {
        cpu_blob_allocator_.clear();
        cpu_workspace_allocator_.clear();
    }
}

#if NCNN_VULKAN
void RealCUGANEngine::setup_vulkan_allocators(int device_id) {
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

void RealCUGANEngine::release_vulkan_allocators() {
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
