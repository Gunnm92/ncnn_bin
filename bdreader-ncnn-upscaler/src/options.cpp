#include "options.hpp"

#include <cxxopts.hpp>
#include <iostream>
#include <string>

namespace {
Options::EngineType parse_engine(const std::string& value) {
    if (value == "realesrgan") {
        return Options::EngineType::RealESRGAN;
    }
    return Options::EngineType::RealCUGAN;
}

Options::Mode parse_mode(const std::string& value) {
    if (value == "stdin") {
        return Options::Mode::Stdin;
    }
    if (value == "batch") {
        return Options::Mode::Batch;
    }
    return Options::Mode::File;
}
} // namespace

bool parse_options(int argc, char** argv, Options& opts) {
    try {
        cxxopts::Options parser("bdreader-ncnn-upscaler", "Unified NCNN upscale CLI");
        parser.positional_help("arguments");
        parser.add_options()
            ("engine", "Engine (realcugan|realesrgan)", cxxopts::value<std::string>()->default_value("realcugan"))
            ("mode", "Mode (file|stdin|batch)", cxxopts::value<std::string>()->default_value("file"))
            ("input", "Input path", cxxopts::value<std::string>()->default_value(""))
            ("output", "Output path", cxxopts::value<std::string>()->default_value(""))
            ("gpu-id", "GPU id (auto, -1, 0, ...)", cxxopts::value<std::string>()->default_value("auto"))
            ("tile-size", "Tile size", cxxopts::value<int>()->default_value("0"))
            ("scale", "Scale factor (realesrgan)", cxxopts::value<int>()->default_value("2"))
            ("noise", "Noise level (realcugan)", cxxopts::value<int>()->default_value("-1"))
            ("quality", "Quality flag (F/E/Q/H)", cxxopts::value<std::string>()->default_value("E"))
            ("model", "RealCUGAN model path", cxxopts::value<std::string>()->default_value("backend/models/realcugan/models-se"))
            ("model-name", "RealESRGAN model name (optional, auto-selects by scale if empty)", cxxopts::value<std::string>()->default_value(""))
            ("format", "Output format", cxxopts::value<std::string>()->default_value("webp"))
            ("max-batch-items", "Max batch items", cxxopts::value<int>()->default_value("8"))
            ("batch-size", "Enable batch stdin mode (protocol v1)", cxxopts::value<int>()->default_value("0"))
            ("keep-alive", "Keep process alive for multiple invocations",
                cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
            ("profiling", "Emit per-image profiling metrics",
                cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
            ("verbose", "Verbose logging",
                cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
            ("help", "Print help");

        auto result = parser.parse(argc, argv);
        if (result.count("help")) {
            std::cout << parser.help() << "\n";
            return false;
        }

        opts.engine = parse_engine(result["engine"].as<std::string>());
        opts.mode = parse_mode(result["mode"].as<std::string>());
        opts.gpu_id = result["gpu-id"].as<std::string>();
        opts.tile_size = result["tile-size"].as<int>();
        opts.scale = result["scale"].as<int>();
        opts.noise_level = result["noise"].as<int>();
        opts.quality = result["quality"].as<std::string>();
        opts.model = result["model"].as<std::string>();
        opts.model_name = result["model-name"].as<std::string>();
        opts.input_path = result["input"].as<std::string>();
        opts.output_path = result["output"].as<std::string>();
        opts.output_format = result["format"].as<std::string>();
        opts.max_batch_items = result["max-batch-items"].as<int>();
        opts.batch_size = result["batch-size"].as<int>();
        opts.keep_alive = result["keep-alive"].as<bool>();
        opts.profiling = result["profiling"].as<bool>();
        opts.verbose = result["verbose"].as<bool>();

        return true;
    } catch (const cxxopts::exceptions::exception& ex) {
        std::cerr << "Invalid arguments: " << ex.what() << "\n";
        return false;
    }
}
