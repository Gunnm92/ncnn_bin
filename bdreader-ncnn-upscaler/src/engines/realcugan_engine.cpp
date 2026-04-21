#include "realcugan_engine.hpp"

#include "../utils/logger.hpp"
#include "net.h"

#include <cctype>
#include <string>

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

void RealCUGANEngine::on_options_loaded() {
    if (current_options_.noise_level < 0) {
        current_options_.noise_level = quality_to_noise(current_options_.quality);
    }
}

std::string RealCUGANEngine::choose_model() const {
    switch (current_options_.noise_level) {
        case -1: return "up2x-no-denoise";
        case 0:  return "up2x-denoise1x";
        case 1:  return "up2x-denoise2x";
        case 2:  return "up2x-denoise3x";
        default: return "up2x-conservative";
    }
}

bool RealCUGANEngine::run_inference_impl(const ncnn::Mat& input, ncnn::Mat& output) {
    // Note: noise_level is baked into the model (up2x-denoise1x, up2x-denoise2x, etc.)
    // and is NOT a dynamic input parameter for these pre-compiled models.
    ncnn::Extractor ex = net_.create_extractor();
    ex.input("in0", input);
    const int ret = ex.extract("out0", output);
    if (ret != 0) {
        logger::warn("RealCUGAN inference returned " + std::to_string(ret));
        return false;
    }
    return true;
}

int RealCUGANEngine::get_scale_factor() const {
    // RealCUGAN up2x-* models are hard-coded to 2x upscaling.
    // Ignoring opts.scale prevents tiling from allocating an oversized output
    // buffer (e.g. 4x) that the model cannot fill.
    return 2;
}
