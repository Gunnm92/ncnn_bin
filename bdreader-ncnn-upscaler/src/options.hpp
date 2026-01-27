#pragma once

#include <string>

struct Options {
    enum class EngineType { RealCUGAN, RealESRGAN };
    enum class Mode { File, Stdin, Batch };

    EngineType engine = EngineType::RealCUGAN;
    Mode mode = Mode::File;
    std::string gpu_id = "auto";
    int tile_size = 0;
    int max_batch_items = 8;
    int scale = 2;
    int noise_level = -1;
    std::string quality = "E";
    std::string model = "backend/models/realcugan/models-se";
    std::string model_name = "";  // Empty by default, will use scale factor to select model
    std::string input_path;
    std::string output_path;
    std::string output_format = "webp";
    int batch_size = 0;  // 0 = disabled, >0 = enable batch stdin mode
    bool verbose = false;
    bool keep_alive = false;
    bool profiling = false;
};

bool parse_options(int argc, char** argv, Options& opts);
