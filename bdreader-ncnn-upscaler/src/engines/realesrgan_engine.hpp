#pragma once

#include "ncnn_upscaler_engine.hpp"

#include <filesystem>
#include <string>

class RealESRGANEngine : public NcnnUpscalerEngine {
public:
    RealESRGANEngine() = default;
    ~RealESRGANEngine() override { cleanup(); }

    int get_scale_factor() const override;

protected:
    const char* engine_name() const override { return "RealESRGAN"; }
    std::filesystem::path default_model_root() const override { return "models/realesrgan"; }
    std::string choose_model() const override;
    std::string fallback_model_name() const override { return "realesr-animevideov3-x2"; }
    bool run_inference_impl(const ncnn::Mat& input, ncnn::Mat& output) override;
};
