#include "batch_mode.hpp"

#include "../utils/logger.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {
constexpr uint32_t kMagic = 0x42445250;
constexpr uint32_t kMaxImageSize = 50 * 1024 * 1024;
struct BatchHeaderStruct {
    uint32_t magic;
    uint32_t version;
    uint32_t num_images;
    uint32_t reserved;
};

bool read_uint32(std::istream& stream, uint32_t& value) {
    stream.read(reinterpret_cast<char*>(&value), sizeof(value));
    return !stream.fail();
}

void write_uint32(std::ostream& stream, uint32_t value) {
    stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

bool read_image(std::istream& stream, std::vector<uint8_t>& data, uint32_t size) {
    data.resize(size);
    if (size == 0) {
        return true;
    }
    stream.read(reinterpret_cast<char*>(data.data()), size);
    return !stream.fail();
}
} // namespace

int run_batch_mode(BaseEngine* engine, const Options& opts) {
    logger::info("Running batch mode");
    if (!engine) {
        logger::error("Engine missing");
        return 1;
    }

    const uint32_t max_items = static_cast<uint32_t>(std::max(1, opts.max_batch_items));

    do {
        BatchHeaderStruct header{};
        if (!read_uint32(std::cin, header.magic)) {
            break; // EOF
        }
        if (!read_uint32(std::cin, header.version) ||
            !read_uint32(std::cin, header.num_images) ||
            !read_uint32(std::cin, header.reserved)) {
            logger::error("Invalid batch header");
            return 1;
        }

        if (header.magic != kMagic) {
            logger::error("Invalid batch magic");
            return 1;
        }

        const uint32_t to_process = std::min(header.num_images, max_items);
        logger::info("Processing batch of " + std::to_string(to_process) + " / " + std::to_string(header.num_images));

        write_uint32(std::cout, kMagic);
        write_uint32(std::cout, header.version);
        write_uint32(std::cout, header.num_images);
        write_uint32(std::cout, 0);

        for (uint32_t i = 0; i < header.num_images; ++i) {
            uint32_t input_size = 0;
            if (!read_uint32(std::cin, input_size)) {
                logger::error("Failed to read batch image size");
                return 1;
            }

            if (input_size > kMaxImageSize) {
                logger::error("Batch image size too large: " + std::to_string(input_size));
                return 1;
            }

            std::vector<uint8_t> input_data;
            if (!read_image(std::cin, input_data, input_size)) {
                logger::error("Failed to read batch image data");
                return 1;
            }

            std::vector<uint8_t> output_data;
            uint32_t status = 1;

            if (i < to_process && input_size > 0) {
                if (engine->process_single(input_data.data(), input_data.size(), output_data, opts.output_format)) {
                    status = 0;
                } else {
                    logger::warn("Batch: failed to process image " + std::to_string(i));
                }
            } else {
                logger::warn("Batch: skipping image " + std::to_string(i) +
                             " (exceeds max-batch-items=" + std::to_string(max_items) + ")");
            }

            const uint32_t size = static_cast<uint32_t>(output_data.size());
            write_uint32(std::cout, status);
            write_uint32(std::cout, size);
            if (size > 0) {
                std::cout.write(reinterpret_cast<const char*>(output_data.data()), size);
            }
        }

        std::cout.flush();
    } while (opts.keep_alive && !std::cin.eof());

    return 0;
}
