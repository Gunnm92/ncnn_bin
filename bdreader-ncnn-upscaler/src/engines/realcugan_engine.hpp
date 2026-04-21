#pragma once

#include "ncnn_upscaler_engine.hpp"

#include <filesystem>
#include <string>

class RealCUGANEngine : public NcnnUpscalerEngine {
public:
    RealCUGANEngine() = default;
    ~RealCUGANEngine() override { cleanup(); }

    int get_scale_factor() const override;

protected:
    const char* engine_name() const override { return "RealCUGAN"; }
    std::filesystem::path default_model_root() const override { return {}; }
    std::string choose_model() const override;
    std::string fallback_model_name() const override { return "up2x-conservative"; }
    bool run_inference_impl(const ncnn::Mat& input, ncnn::Mat& output) override;
    void on_options_loaded() override;
};
