#include "stdin_mode.hpp"

#include "../utils/logger.hpp"
#include "../utils/blocking_queue.hpp"
#include "protocol_v2.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
std::vector<uint8_t> read_all_stream(std::istream& stream) {
    std::vector<uint8_t> buffer;
    std::array<char, 4096> chunk;
    while (stream.read(chunk.data(), chunk.size())) {
        buffer.insert(buffer.end(), chunk.begin(), chunk.end());
    }
    buffer.insert(buffer.end(), chunk.begin(), chunk.begin() + stream.gcount());
    return buffer;
}

using namespace protocol_v2;

// Read exact number of bytes from stdin
bool read_exact(std::istream& stream, uint8_t* buffer, size_t size) {
    size_t total_read = 0;
    while (total_read < size) {
        stream.read(reinterpret_cast<char*>(buffer + total_read), size - total_read);
        if (stream.eof() || stream.fail()) {
            return false;
        }
        total_read += stream.gcount();
    }
    return true;
}

// Read uint32_t in little-endian
bool read_u32(std::istream& stream, uint32_t& value) {
    uint8_t bytes[4];
    if (!read_exact(stream, bytes, 4)) {
        return false;
    }
    value = static_cast<uint32_t>(bytes[0]) |
            (static_cast<uint32_t>(bytes[1]) << 8) |
            (static_cast<uint32_t>(bytes[2]) << 16) |
            (static_cast<uint32_t>(bytes[3]) << 24);
    return true;
}

// Write uint32_t in little-endian
void write_u32(std::ostream& stream, uint32_t value) {
    uint8_t bytes[4];
    bytes[0] = value & 0xFF;
    bytes[1] = (value >> 8) & 0xFF;
    bytes[2] = (value >> 16) & 0xFF;
    bytes[3] = (value >> 24) & 0xFF;
    stream.write(reinterpret_cast<const char*>(bytes), 4);
}

void write_protocol_response(std::ostream& stream,
                             uint32_t request_id,
                             ProtocolStatus status,
                             const std::string& error_message,
                             const std::vector<std::vector<uint8_t>>& outputs) {
    const uint32_t error_len = static_cast<uint32_t>(error_message.size());
    uint32_t payload_bytes = 4 + 4 + 4 + error_len + 4;
    uint32_t outputs_bytes = 0;
    for (const auto& output : outputs) {
        outputs_bytes += 4 + static_cast<uint32_t>(output.size());
    }
    payload_bytes += outputs_bytes;

    write_u32(stream, payload_bytes);
    write_u32(stream, request_id);
    write_u32(stream, static_cast<uint32_t>(status));
    write_u32(stream, error_len);
    if (error_len > 0) {
        stream.write(error_message.data(), error_len);
    }
    write_u32(stream, static_cast<uint32_t>(outputs.size()));
    for (const auto& output : outputs) {
        write_u32(stream, static_cast<uint32_t>(output.size()));
        if (!output.empty()) {
            stream.write(reinterpret_cast<const char*>(output.data()), output.size());
        }
    }
    stream.flush();
}

void write_protocol_error(std::ostream& stream,
                          uint32_t request_id,
                          ProtocolStatus status,
                          const std::string& message) {
    write_protocol_response(stream, request_id, status, message, {});
}

struct ProtocolMetrics {
    std::atomic<uint32_t> processed{0};
    std::atomic<uint32_t> errors{0};
    std::atomic<uint64_t> total_ns{0};
};

bool discard_bytes(std::istream& stream, size_t bytes_to_discard) {
    constexpr size_t kChunk = 4096;
    std::array<uint8_t, kChunk> buffer{};
    while (bytes_to_discard > 0) {
        const size_t chunk = std::min(bytes_to_discard, kChunk);
        if (!read_exact(stream, buffer.data(), chunk)) {
            return false;
        }
        bytes_to_discard -= chunk;
    }
    return true;
}

struct MemorySample {
    size_t rss_kb = 0;
    size_t hwm_kb = 0;
};

MemorySample read_process_memory_kb() {
    MemorySample sample;
    std::ifstream status("/proc/self/status");
    if (!status.is_open()) {
        return sample;
    }
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            std::istringstream iss(line.substr(6));
            iss >> sample.rss_kb;
        } else if (line.rfind("VmHWM:", 0) == 0) {
            std::istringstream iss(line.substr(6));
            iss >> sample.hwm_kb;
        }
    }
    return sample;
}

// ============================================================================
// Pipeline Streaming Multi-Thread Batch Processing (v4)
// ============================================================================

/// Input item for pipeline (compressed image from stdin)
struct InputItem {
    uint32_t id;                    // Image index (0-based)
    std::vector<uint8_t> data;      // Compressed JPEG/PNG bytes (2-5MB)

    InputItem() : id(0) {}
    InputItem(uint32_t _id, std::vector<uint8_t>&& _data)
        : id(_id), data(std::move(_data)) {}
};

/// Output item for pipeline (compressed result to stdout)
struct OutputItem {
    uint32_t id;                    // Image index (for ordering)
    std::vector<uint8_t> data;      // Compressed WebP bytes (2-10MB)

    OutputItem() : id(0) {}
    OutputItem(uint32_t _id, std::vector<uint8_t>&& _data)
        : id(_id), data(std::move(_data)) {}
};

struct PipelineMetrics {
    std::atomic<uint32_t> processed{0};
    std::atomic<uint32_t> errors{0};
    std::atomic<uint64_t> input_bytes{0};
    std::atomic<uint64_t> output_bytes{0};
    std::atomic<uint64_t> total_ns{0};
};

/// Thread 1: Reader (Producer)
/// Reads compressed images from stdin and pushes to InputQueue
void reader_thread_func(
    BoundedBlockingQueue<InputItem>& input_queue,
    uint32_t num_images,
    std::atomic<bool>& error_flag
) {
    try {
        logger::info("Reader thread started: reading " + std::to_string(num_images) + " images from stdin");

        for (uint32_t i = 0; i < num_images; ++i) {
            // Read image size
            uint32_t image_size = 0;
            if (!read_u32(std::cin, image_size)) {
                logger::error("Reader: Failed to read image_size for image " + std::to_string(i));
                error_flag = true;
                return;
            }

            if (image_size == 0 || image_size > 50 * 1024 * 1024) {
                logger::error("Reader: Invalid image_size for image " + std::to_string(i) + ": " + std::to_string(image_size));
                error_flag = true;
                return;
            }

            // Read compressed image data
            std::vector<uint8_t> image_data;
            image_data.resize(image_size);
            if (!read_exact(std::cin, image_data.data(), image_size)) {
                logger::error("Reader: Failed to read image data for image " + std::to_string(i));
                error_flag = true;
                return;
            }

            // Push to queue (blocks if queue full - backpressure)
            input_queue.push(InputItem(i, std::move(image_data)));

            logger::info("Reader: Image " + std::to_string(i + 1) + "/" + std::to_string(num_images) +
                        " read (" + std::to_string(image_size) + " bytes, queue size=" +
                        std::to_string(input_queue.size()) + "/" + std::to_string(input_queue.capacity()) + ")");
        }

        logger::info("Reader thread finished: closing input queue");
        input_queue.close();

    } catch (const std::exception& e) {
        logger::error("Reader thread exception: " + std::string(e.what()));
        error_flag = true;
        input_queue.close();
    }
}

/// Thread 2: Worker (Consumer/Producer)
/// Pops from InputQueue, processes on GPU, pushes to OutputQueue
void worker_thread_func(
    BoundedBlockingQueue<InputItem>& input_queue,
    BoundedBlockingQueue<OutputItem>& output_queue,
    BaseEngine* engine,
    const std::string& output_format,
    std::atomic<bool>& error_flag,
    PipelineMetrics& metrics,
    bool log_memory
) {
    try {
        logger::info("Worker thread started: GPU processing loop");

        uint32_t processed_count = 0;
        uint32_t consecutive_errors = 0;  // Track consecutive errors for monitoring
        InputItem input_item;

        while (input_queue.pop(input_item)) {
            try {
                logger::info("Worker: Starting image " + std::to_string(input_item.id) +
                            " (input size=" + std::to_string(input_item.data.size()) + " bytes)");

                if (log_memory) {
                    const auto mem = read_process_memory_kb();
                    logger::info("Worker: mem before image " + std::to_string(input_item.id) +
                                 " rss_kb=" + std::to_string(mem.rss_kb) +
                                 " hwm_kb=" + std::to_string(mem.hwm_kb));
                }

                // Process image on GPU
                // Note: RGB uncompressed buffers are local scope - freed after each iteration
                std::vector<uint8_t> output_data;
                const auto start = std::chrono::steady_clock::now();
                
                bool success = engine->process_single(
                    input_item.data.data(), 
                    input_item.data.size(),
                    output_data, 
                    output_format
                );
                
                if (!success) {
                    logger::error("Worker: Failed to process image " + std::to_string(input_item.id));
                    metrics.errors.fetch_add(1, std::memory_order_relaxed);

                    // Track consecutive errors for monitoring.
                    consecutive_errors++;

                    // Free input buffer before continuing
                    // Note: clear() without shrink_to_fit() avoids memory fragmentation
                    // The buffer capacity is preserved for reuse in next iteration (Ring Buffer pattern)
                    input_item.data.clear();
                    // Continue processing next image instead of breaking
                    continue;
                }

                const auto duration = std::chrono::steady_clock::now() - start;
                metrics.total_ns.fetch_add(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count(),
                    std::memory_order_relaxed);
                metrics.processed.fetch_add(1, std::memory_order_relaxed);

                // Do NOT call engine->cleanup() here.
                // Cleanup inside a loop destroys the model and breaks subsequent images.
                metrics.input_bytes.fetch_add(input_item.data.size(), std::memory_order_relaxed);
                metrics.output_bytes.fetch_add(output_data.size(), std::memory_order_relaxed);

                // Reset consecutive error counter on success
                consecutive_errors = 0;

                logger::info("Worker: Image " + std::to_string(input_item.id) + " processed, output size=" +
                            std::to_string(output_data.size()) + " bytes");

                if (log_memory) {
                    const auto mem = read_process_memory_kb();
                    logger::info("Worker: mem after image " + std::to_string(input_item.id) +
                                 " rss_kb=" + std::to_string(mem.rss_kb) +
                                 " hwm_kb=" + std::to_string(mem.hwm_kb));
                }

                // NOTE: Do NOT call cleanup() here - it corrupts the NCNN model!
                // Cleanup will be called once at the end of the batch instead.

                // Free input buffer BEFORE pushing output (reduce peak memory)
                // Note: clear() without shrink_to_fit() avoids memory fragmentation
                // The buffer capacity is preserved for reuse in next iteration (Ring Buffer pattern)
                input_item.data.clear();

                // Push to output queue (blocks if full - backpressure)
                // If push throws, output_data will be automatically freed by RAII
                output_queue.push(OutputItem(input_item.id, std::move(output_data)));

                processed_count++;
                logger::info("Worker: Image " + std::to_string(input_item.id) + " queued for writing (" +
                            std::to_string(processed_count) + " total processed)");

            } catch (const std::exception& e) {
                // Per-image exception: log and continue with next image
                logger::error("Worker: Exception processing image " + std::to_string(input_item.id) +
                             ": " + std::string(e.what()));
                metrics.errors.fetch_add(1, std::memory_order_relaxed);

                // Track consecutive errors for monitoring.
                consecutive_errors++;

                // Free buffers (RAII will handle, but explicit is clear)
                // Note: clear() without shrink_to_fit() avoids memory fragmentation
                input_item.data.clear();
                // Continue processing next image instead of breaking
                continue;
            }
        }

        logger::info("Worker thread finished: processed " + std::to_string(processed_count) + " images");

        // Cleanup GPU memory ONCE at the end of the batch
        // This is safe because we're done processing all images
        logger::info("Worker: Cleaning up GPU memory (end of batch)");
        engine->cleanup();

        output_queue.close();

    } catch (const std::exception& e) {
        logger::error("Worker thread exception: " + std::string(e.what()));
        error_flag = true;
        output_queue.close();
    } catch (...) {
        logger::error("Worker thread unknown exception");
        error_flag = true;
        output_queue.close();
    }
}

/// Thread 3: Writer (Consumer)
/// Pops from OutputQueue and writes to stdout
void writer_thread_func(
    BoundedBlockingQueue<OutputItem>& output_queue,
    std::atomic<bool>& error_flag
) {
    try {
        logger::info("Writer thread started: streaming results to stdout");

        uint32_t written_count = 0;
        OutputItem output_item;

        while (output_queue.pop(output_item)) {
            // Write result to stdout
            write_u32(std::cout, static_cast<uint32_t>(output_item.data.size()));
            std::cout.write(reinterpret_cast<const char*>(output_item.data.data()), output_item.data.size());
            std::cout.flush();

            written_count++;
            logger::info("Writer: Image " + std::to_string(output_item.id) + " written (" +
                        std::to_string(written_count) + " total, " +
                        std::to_string(output_item.data.size()) + " bytes)");

            // output_item.data is automatically freed here (scope end)
        }

        logger::info("Writer thread finished: wrote " + std::to_string(written_count) + " results");

    } catch (const std::exception& e) {
        logger::error("Writer thread exception: " + std::string(e.what()));
        error_flag = true;
    }
}

} // namespace

int run_keep_alive_protocol_v2(BaseEngine* engine, const Options& opts) {
    constexpr uint32_t kMaxMessageBytes = 64u * 1024u * 1024u;
    uint32_t handled = 0;
    ProtocolMetrics metrics;
    const bool log_protocol = opts.log_protocol;

    logger::info("Protocol v2 keep-alive loop started (magic=BRDR version=2, max_message_bytes=" +
                 std::to_string(kMaxMessageBytes) + ")");

    auto record_outcome = [&](uint32_t request_id,
                              ProtocolStatus status,
                              const std::string& error_message,
                              size_t result_count,
                              const std::chrono::steady_clock::time_point& start,
                              size_t bytes_in,
                              size_t bytes_out,
                              const RequestPayload* request_info) {
        const auto elapsed_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
        metrics.total_ns.fetch_add(elapsed_ns, std::memory_order_relaxed);
        if (status == ProtocolStatus::Ok) {
            metrics.processed.fetch_add(1, std::memory_order_relaxed);
        } else {
            metrics.errors.fetch_add(1, std::memory_order_relaxed);
        }

        if (log_protocol) {
            std::ostringstream oss;
            oss << "Protocol v2 response request_id=" << request_id
                << " status=" << static_cast<uint32_t>(status)
                << " elapsed_ms=" << std::fixed << std::setprecision(2) << (elapsed_ns / 1e6)
                << " results=" << result_count;
            if (!error_message.empty()) {
                oss << " error='" << error_message << "'";
            }
            logger::info(oss.str());
        }

        if (opts.profiling) {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(2);
            oss << "Profiling request_id=" << request_id
                << " status=" << static_cast<uint32_t>(status);
            if (request_info) {
                const auto engine_name = (request_info->engine == Options::EngineType::RealESRGAN)
                                             ? "RealESRGAN"
                                             : "RealCUGAN";
                oss << " engine=" << engine_name
                    << " quality_or_scale='" << request_info->quality_or_scale << "'"
                    << " gpu_id=" << request_info->gpu_id
                    << " batch_count=" << request_info->batch_count;
            }
            oss << " results=" << result_count
                << " bytes_in=" << bytes_in
                << " bytes_out=" << bytes_out
                << " elapsed_ms=" << (elapsed_ns / 1e6);
            if (!error_message.empty()) {
                oss << " error_len=" << error_message.size() << " error='" << error_message << "'";
            }
            logger::info(oss.str());
        }
    };

    while (true) {
        uint32_t message_len = 0;
        if (!read_u32(std::cin, message_len)) {
            logger::info("Protocol v2 stream closed by peer");
            break;
        }

        const auto frame_start = std::chrono::steady_clock::now();

        if (message_len == 0) {
            logger::info("Received shutdown frame (message_len=0)");
            break;
        }

        if (message_len < kProtocolHeaderSize) {
            logger::error("Protocol v2 frame too small: " + std::to_string(message_len));
            if (!discard_bytes(std::cin, message_len)) {
                logger::error("Failed to discard undersized frame data");
                break;
            }
            write_protocol_error(std::cout, 0, ProtocolStatus::InvalidFrame, "frame too short for header");
            record_outcome(0,
                           ProtocolStatus::InvalidFrame,
                           "frame too short for header",
                           0,
                           frame_start,
                           message_len,
                           0,
                           nullptr);
            continue;
        }

        if (message_len > kMaxMessageBytes) {
            logger::error("Protocol v2 frame too large: " + std::to_string(message_len));
            if (!discard_bytes(std::cin, message_len)) {
                logger::error("Failed to discard oversized frame data");
                break;
            }
            write_protocol_error(std::cout, 0, ProtocolStatus::InvalidFrame, "frame exceeds max size");
            record_outcome(0,
                           ProtocolStatus::InvalidFrame,
                           "frame exceeds max size",
                           0,
                           frame_start,
                           message_len,
                           0,
                           nullptr);
            continue;
        }

        std::vector<uint8_t> payload(message_len);
        if (!read_exact(std::cin, payload.data(), payload.size())) {
            logger::error("Failed to read protocol v2 payload (" + std::to_string(message_len) + " bytes)");
            break;
        }

        ProtocolHeader header;
        std::string header_error;
        if (!parse_protocol_header(payload.data(), payload.size(), header, header_error)) {
            logger::error("Protocol header validation failed: " + header_error);
            write_protocol_error(std::cout, 0, ProtocolStatus::ValidationError, header_error);
            record_outcome(0,
                           ProtocolStatus::ValidationError,
                           header_error,
                           0,
                           frame_start,
                           message_len,
                           0,
                           nullptr);
            continue;
        }

        if (header.msg_type != static_cast<uint8_t>(ProtocolMessageType::Request)) {
            const std::string message = "only request frames accepted";
            logger::error("Protocol v2 message_type=" + std::to_string(header.msg_type) +
                          " not supported; only request frames are allowed");
            write_protocol_error(std::cout, header.request_id, ProtocolStatus::ValidationError, message);
            record_outcome(header.request_id,
                           ProtocolStatus::ValidationError,
                           message,
                           0,
                           frame_start,
                           message_len,
                           0,
                           nullptr);
            continue;
        }

        const size_t body_size = payload.size() - kProtocolHeaderSize;
        const uint8_t* body_ptr = payload.data() + kProtocolHeaderSize;

        if (body_size == 0) {
            const std::string message = "request body empty";
            logger::warn("Protocol v2 request_id=" + std::to_string(header.request_id) + " has empty body");
            write_protocol_error(std::cout, header.request_id, ProtocolStatus::ValidationError, message);
            record_outcome(header.request_id,
                           ProtocolStatus::ValidationError,
                           message,
                           0,
                           frame_start,
                           message_len,
                           0,
                           nullptr);
            continue;
        }

        RequestPayload request;
        ProtocolStatus payload_status = ProtocolStatus::ValidationError;
        if (!parse_request_payload(body_ptr, body_size, opts.max_batch_items, request, header_error, payload_status)) {
            logger::error("Protocol v2 request_id=" + std::to_string(header.request_id) +
                          " payload parse failed: " + header_error);
            write_protocol_error(std::cout, header.request_id, payload_status, header_error);
            record_outcome(header.request_id,
                           payload_status,
                           header_error,
                           0,
                           frame_start,
                           message_len,
                           0,
                           nullptr);
            continue;
        }

        logger::info("Protocol v2 request_id=" + std::to_string(header.request_id) +
                     " engine=" + (request.engine == Options::EngineType::RealESRGAN ? "RealESRGAN" : "RealCUGAN") +
                     " quality_or_scale='" + request.quality_or_scale +
                     "' gpu_id=" + std::to_string(request.gpu_id) +
                     " batch_count=" + std::to_string(request.batch_count));

        std::vector<std::vector<uint8_t>> outputs;
        outputs.reserve(request.batch_count);
        bool failed = false;

        for (uint32_t i = 0; i < request.batch_count; ++i) {
            const auto& image = request.images[i];
            std::vector<uint8_t> output;
            if (!engine->process_single(image.data(), image.size(), output, opts.output_format)) {
                const std::string error_msg =
                    "engine processing failed at index " + std::to_string(i);
                logger::error("Engine failed processing request_id=" + std::to_string(header.request_id) +
                              " image index=" + std::to_string(i));
                write_protocol_error(std::cout, header.request_id, ProtocolStatus::EngineError, error_msg);
                record_outcome(header.request_id,
                               ProtocolStatus::EngineError,
                               error_msg,
                               i,
                               frame_start,
                               message_len,
                               0,
                               &request);
                failed = true;
                break;
            }
            outputs.push_back(std::move(output));
        }

        if (failed) {
            continue;
        }

        size_t output_bytes = 0;
        for (const auto& output : outputs) {
            output_bytes += output.size();
        }

        write_protocol_response(std::cout, header.request_id, ProtocolStatus::Ok, "", outputs);
        record_outcome(header.request_id,
                       ProtocolStatus::Ok,
                       "",
                       outputs.size(),
                       frame_start,
                       message_len,
                       output_bytes,
                       &request);
        ++handled;
    }

    const uint32_t processed = metrics.processed.load(std::memory_order_relaxed);
    const uint32_t errors = metrics.errors.load(std::memory_order_relaxed);
    const uint64_t total_ns = metrics.total_ns.load(std::memory_order_relaxed);
    if (processed || errors) {
        const double avg_ms = processed ? (total_ns / double(processed)) / 1e6 : 0.0;
        std::ostringstream summary;
        summary << std::fixed << std::setprecision(2);
        summary << "Protocol v2 summary: processed=" << processed
                << ", errors=" << errors
                << ", avg_latency_ms=" << avg_ms;
        logger::info(summary.str());
    }

    logger::info("Protocol v2 keep-alive loop exiting after " + std::to_string(handled) + " frames");
    return 0;
}

int run_stdin_mode(BaseEngine* engine, const Options& opts) {
    logger::info("Running stdin mode");
    if (!engine) {
        logger::error("Engine missing");
        return 1;
    }

    // Legacy single-image mode: reads stdin until EOF then writes raw output bytes to stdout.
    // NOTE: In this mode, the caller must close stdin (send EOF) before the process can start
    // processing; otherwise the process will block waiting for more input.
    if (!opts.keep_alive) {
        auto input = read_all_stream(std::cin);
        if (input.empty()) {
            return 0;
        }

        std::vector<uint8_t> output;
        if (!engine->process_single(input.data(), input.size(), output, opts.output_format)) {
            logger::error("Failed to process stdin payload");
            return 1;
        }

        std::cout.write(reinterpret_cast<const char*>(output.data()), output.size());
        std::cout.flush();
        return 0;
    }

    // Keep-alive framed mode (streaming without EOF) using protocol v2.
    logger::info("--keep-alive enabled; using protocol v2 framing");
    return run_keep_alive_protocol_v2(engine, opts);
}
