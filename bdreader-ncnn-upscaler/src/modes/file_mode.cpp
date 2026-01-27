#include "file_mode.hpp"

#include "../utils/logger.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>

namespace {
std::vector<uint8_t> read_entire_file(const std::string& path) {
    logger::info("Reading file from: " + path);
    logger::info("Exists: " + std::to_string(std::filesystem::exists(path)));
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        logger::warn("Cannot open file: " + path);
        return {};
    }
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
}

bool write_entire_file(const std::string& path, const std::vector<uint8_t>& data) {
    if (path.empty()) {
        return false;
    }
    std::filesystem::path output(path);
    if (auto dir = output.parent_path(); !dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
    }

    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    file.write(reinterpret_cast<const char*>(data.data()), data.size());
    return file.good();
}
} // namespace

int run_file_mode(BaseEngine* engine, const Options& opts) {
    logger::info("Running file mode");
    if (!engine) {
        logger::error("Engine missing");
        return 1;
    }

    if (opts.input_path.empty() || opts.output_path.empty()) {
        logger::error("File mode requires --input and --output");
        return 1;
    }

    const auto input_data = read_entire_file(opts.input_path);
    if (input_data.empty()) {
        logger::error("Failed to read input file: " + opts.input_path);
        return 1;
    }

    std::vector<uint8_t> output_data;
    if (!engine->process_single(input_data.data(), input_data.size(), output_data, opts.output_format)) {
        logger::error("Engine failed to process file");
        return 1;
    }

    if (!write_entire_file(opts.output_path, output_data)) {
        logger::error("Failed to write output file: " + opts.output_path);
        return 1;
    }

    logger::info("File mode completed: " + opts.output_path);
    return 0;
}
