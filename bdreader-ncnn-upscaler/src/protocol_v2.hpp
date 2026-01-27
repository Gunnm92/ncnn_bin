#pragma once

#include "options.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace protocol_v2 {

constexpr uint32_t kProtocolMagic = 0x42524452; // 'BRDR'
constexpr uint8_t kProtocolVersion = 2;
constexpr size_t kProtocolHeaderSize = 4 + 1 + 1 + 4;
constexpr size_t kMaxMetaStringBytes = 64;
constexpr uint32_t kMaxImageSizeBytes = 50u * 1024u * 1024u;

enum class ProtocolMessageType : uint8_t {
    Request = 1,
    Response = 2,
};

enum class ProtocolStatus : uint32_t {
    Ok = 0,
    InvalidFrame = 1,
    ValidationError = 2,
    EngineError = 3,
};

struct ProtocolHeader {
    uint32_t magic;
    uint8_t version;
    uint8_t msg_type;
    uint32_t request_id;
};

struct RequestPayload {
    Options::EngineType engine;
    std::string quality_or_scale;
    int32_t gpu_id;
    uint32_t batch_count;
    std::vector<std::vector<uint8_t>> images;
};

constexpr uint32_t decode_u32_le(const uint8_t* ptr) {
    return static_cast<uint32_t>(ptr[0]) |
           (static_cast<uint32_t>(ptr[1]) << 8) |
           (static_cast<uint32_t>(ptr[2]) << 16) |
           (static_cast<uint32_t>(ptr[3]) << 24);
}

inline bool read_le_u32(const uint8_t*& ptr, size_t& remaining, uint32_t& value) {
    if (remaining < 4) {
        return false;
    }
    value = decode_u32_le(ptr);
    ptr += 4;
    remaining -= 4;
    return true;
}

inline bool read_le_i32(const uint8_t*& ptr, size_t& remaining, int32_t& value) {
    uint32_t raw;
    if (!read_le_u32(ptr, remaining, raw)) {
        return false;
    }
    value = static_cast<int32_t>(raw);
    return true;
}

inline bool parse_protocol_header(const uint8_t* payload,
                                  size_t payload_size,
                                  ProtocolHeader& header,
                                  std::string& error) {
    if (payload_size < kProtocolHeaderSize) {
        error = "payload too small for protocol header";
        return false;
    }

    header.magic = decode_u32_le(payload);
    header.version = payload[4];
    header.msg_type = payload[5];
    header.request_id = decode_u32_le(payload + 6);

    if (header.magic != kProtocolMagic) {
        error = "invalid magic, expected BRDR";
        return false;
    }
    if (header.version != kProtocolVersion) {
        error = "unsupported protocol version " + std::to_string(header.version);
        return false;
    }

    return true;
}

inline bool parse_request_payload(const uint8_t* data,
                                  size_t size,
                                  size_t max_batch_items,
                                  RequestPayload& request,
                                  std::string& error) {
    const uint8_t* ptr = data;
    size_t remaining = size;

    if (remaining < 1) {
        error = "missing engine enum";
        return false;
    }
    const uint8_t engine_id = *ptr++;
    remaining -= 1;

    if (engine_id > 1) {
        error = "engine enum must be 0 (RealCUGAN) or 1 (RealESRGAN)";
        return false;
    }
    request.engine = (engine_id == 1) ? Options::EngineType::RealESRGAN : Options::EngineType::RealCUGAN;

    uint32_t meta_len = 0;
    if (!read_le_u32(ptr, remaining, meta_len)) {
        error = "incomplete quality/scale length";
        return false;
    }
    if (meta_len > kMaxMetaStringBytes) {
        error = "quality/scale metadata too long";
        return false;
    }
    if (meta_len > remaining) {
        error = "quality/scale metadata truncated";
        return false;
    }
    request.quality_or_scale.assign(reinterpret_cast<const char*>(ptr), meta_len);
    ptr += meta_len;
    remaining -= meta_len;

    if (!read_le_i32(ptr, remaining, request.gpu_id)) {
        error = "missing gpu_id";
        return false;
    }

    if (!read_le_u32(ptr, remaining, request.batch_count)) {
        error = "missing batch_count";
        return false;
    }

    if (request.batch_count == 0) {
        error = "batch_count must be positive";
        return false;
    }

    if (request.batch_count > max_batch_items) {
        error = "batch_count exceeds --max-batch-items";
        return false;
    }

    request.images.clear();
    request.images.reserve(request.batch_count);
    for (uint32_t i = 0; i < request.batch_count; ++i) {
        uint32_t image_len = 0;
        if (!read_le_u32(ptr, remaining, image_len)) {
            error = "missing image length for entry " + std::to_string(i);
            return false;
        }
        if (image_len > kMaxImageSizeBytes) {
            error = "image size exceeds limit: " + std::to_string(image_len);
            return false;
        }
        if (image_len > remaining) {
            error = "image payload truncated for entry " + std::to_string(i);
            return false;
        }
        request.images.emplace_back(ptr, ptr + image_len);
        ptr += image_len;
        remaining -= image_len;
    }

    if (remaining > 0) {
        error = "trailing bytes after images";
        return false;
    }

    return true;
}

} // namespace protocol_v2
