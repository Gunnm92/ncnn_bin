#include "realesrgan_engine.hpp"

#include "../utils/logger.hpp"
#include "net.h"

#include <string>

std::string RealESRGANEngine::choose_model() const {
    if (!current_options_.model_name.empty()) {
        logger::info("RealESRGAN using model_name: " + current_options_.model_name);
        return current_options_.model_name;
    }

    logger::info("RealESRGAN selecting model by scale: " + std::to_string(current_options_.scale));
    switch (current_options_.scale) {
        case 2: return "realesr-animevideov3-x2";
        case 3: return "realesr-animevideov3-x3";
        case 4: return "realesr-animevideov3-x4";
        default:
            logger::warn("RealESRGAN unexpected scale " + std::to_string(current_options_.scale) +
                         ", defaulting to x2");
            return "realesr-animevideov3-x2";
    }
}

bool RealESRGANEngine::run_inference_impl(const ncnn::Mat& input, ncnn::Mat& output) {
    ncnn::Extractor ex = net_.create_extractor();

    // Try "data" first (realesr-animevideov3 models); fall back to "in0" (realesr-general).
    int ret = ex.input("data", input);
    if (ret != 0) {
        ex = net_.create_extractor();
        ret = ex.input("in0", input);
        if (ret != 0) {
            logger::error("RealESRGAN failed to find input blob (tried 'data' and 'in0')");
            return false;
        }
        ret = ex.extract("out0", output);
    } else {
        ret = ex.extract("output", output);
    }

    if (ret != 0) {
        logger::warn("RealESRGAN inference returned " + std::to_string(ret));
        return false;
    }
    return true;
}

int RealESRGANEngine::get_scale_factor() const {
    return current_options_.scale;
}
