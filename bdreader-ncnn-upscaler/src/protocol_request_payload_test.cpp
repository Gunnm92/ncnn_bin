#include "protocol_v2.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

void append_u32(std::vector<uint8_t>& buffer, uint32_t value) {
    buffer.push_back(static_cast<uint8_t>(value & 0xFF));
    buffer.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    buffer.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

void append_i32(std::vector<uint8_t>& buffer, int32_t value) {
    append_u32(buffer, static_cast<uint32_t>(value));
}

std::vector<uint8_t> build_payload(uint8_t engine,
                                   const std::string& meta,
                                   int32_t gpu_id,
                                   uint32_t batch_count,
                                   const std::vector<std::vector<uint8_t>>& images) {
    std::vector<uint8_t> payload;
    payload.push_back(engine);
    append_u32(payload, static_cast<uint32_t>(meta.size()));
    payload.insert(payload.end(), meta.begin(), meta.end());
    append_i32(payload, gpu_id);
    append_u32(payload, batch_count);
    for (const auto& image : images) {
        append_u32(payload, static_cast<uint32_t>(image.size()));
        payload.insert(payload.end(), image.begin(), image.end());
    }
    return payload;
}

} // namespace

int main() {
    using namespace protocol_v2;

    std::vector<std::vector<uint8_t>> images = {
        {0x01, 0x02, 0x03},
        {0xAA, 0xBB, 0xCC, 0xDD}
    };

    auto valid_payload = build_payload(0, "E", -1, 2, images);
    RequestPayload request;
    std::string error;
    if (!parse_request_payload(valid_payload.data(), valid_payload.size(), 8, request, error)) {
        std::cerr << "Valid request rejected: " << error << "\n";
        return 1;
    }
    if (request.batch_count != 2 || request.images.size() != 2) {
        std::cerr << "Parsed batch_count mismatch\n";
        return 1;
    }

    auto overflow_payload = build_payload(0, "E", 0, 9, images);
    if (parse_request_payload(overflow_payload.data(), overflow_payload.size(), 8, request, error)) {
        std::cerr << "Overflow batch_count accepted\n";
        return 1;
    }
    if (error.find("batch_count") == std::string::npos) {
        std::cerr << "Error text missing for overflow case\n";
        return 1;
    }

    std::cout << "protocol_request_payload_test passed\n";
    return 0;
}
