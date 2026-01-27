#include "stdin_mode.hpp"

#include "../utils/logger.hpp"
#include "../utils/blocking_queue.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
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

constexpr uint32_t kMaxImageSizeBytes = 50u * 1024u * 1024u;

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

int run_stdin_mode(BaseEngine* engine, const Options& opts) {
    logger::info("Running stdin mode");
    if (!engine) {
        logger::error("Engine missing");
        return 1;
    }

    // Check if batch mode is requested (protocol v1)
    // Protocol: [num_images:u32][size1:u32][data1][size2:u32][data2]...
    if (opts.batch_size > 0) {
        logger::info("Batch stdin mode enabled (batch_size=" + std::to_string(opts.batch_size) + ")");
        return run_batch_stdin(engine, opts);
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

    // Keep-alive framed mode (streaming without EOF):
    // stdin  : [input_size:u32_le][input_bytes...]
    // stdout : [status:u32_le][output_size:u32_le][output_bytes...]
    // Where status=0 on success, non-zero on error. input_size=0 cleanly ends the loop.
    logger::info("--keep-alive enabled; using framed stdin/stdout protocol");

    while (true) {
        uint32_t input_size = 0;
        if (!read_u32(std::cin, input_size)) {
            break; // EOF
        }

        if (input_size == 0) {
            break;
        }

        if (input_size > kMaxImageSizeBytes) {
            logger::error("stdin frame too large: " + std::to_string(input_size));
            return 1;
        }

        std::vector<uint8_t> input;
        input.resize(input_size);
        if (!read_exact(std::cin, input.data(), input.size())) {
            logger::error("Failed to read stdin frame payload (" + std::to_string(input_size) + " bytes)");
            return 1;
        }

        std::vector<uint8_t> output;
        uint32_t status = 0;
        if (!engine->process_single(input.data(), input.size(), output, opts.output_format)) {
            logger::error("Failed to process stdin frame payload");
            status = 1;
            output.clear();
        }

        write_u32(std::cout, status);
        write_u32(std::cout, static_cast<uint32_t>(output.size()));
        if (!output.empty()) {
            std::cout.write(reinterpret_cast<const char*>(output.data()), output.size());
        }
        std::cout.flush();
    }

    return 0;
}

// Batch stdin processing using Pipeline Streaming Multi-Thread (v4)
// Architecture: Reader → InputQueue → Worker → OutputQueue → Writer
// Reference: Producer-Consumer pattern with bounded blocking queues
void log_pipeline_metrics(const PipelineMetrics& metrics) {
    const uint32_t processed = metrics.processed.load(std::memory_order_relaxed);
    const uint32_t errors = metrics.errors.load(std::memory_order_relaxed);
    const uint64_t total_ns = metrics.total_ns.load(std::memory_order_relaxed);
    const uint64_t input_bytes = metrics.input_bytes.load(std::memory_order_relaxed);
    const uint64_t output_bytes = metrics.output_bytes.load(std::memory_order_relaxed);

    if (processed == 0 && errors == 0) {
        return;
    }

    const double avg_ms = processed ? (total_ns / double(processed)) / 1e6 : 0.0;
    const double input_mb = input_bytes / double(1024 * 1024);
    const double output_mb = output_bytes / double(1024 * 1024);

    std::ostringstream summary;
    summary << std::fixed << std::setprecision(2);
    summary << "Batch pipeline summary: processed=" << processed
            << ", errors=" << errors
            << ", avg_latency_ms=" << avg_ms
            << ", input_mb=" << input_mb
            << ", output_mb=" << output_mb;

    logger::info(summary.str());
}

int run_batch_stdin(BaseEngine* engine, const Options& opts) {
    // Read number of images header
    uint32_t num_images = 0;
    if (!read_u32(std::cin, num_images)) {
        logger::error("Failed to read num_images from stdin");
        return 1;
    }

    if (num_images == 0 || num_images > 1000) {
        logger::error("Invalid num_images: " + std::to_string(num_images));
        return 1;
    }

    logger::info("Batch processing " + std::to_string(num_images) + " images (Pipeline Streaming Multi-Thread v4)");

    // Write result count header immediately (protocol v4)
    write_u32(std::cout, num_images);
    std::cout.flush();

    // Create bounded queues with optimal capacity for parallelism
    // Capacity=4 allows better overlap between Reader/Worker/Writer threads
    // InputQueue: 4 × 5MB = 20MB (compressed images)
    // OutputQueue: 4 × 10MB = 40MB (compressed results)
    // Worker scope: ~30MB RGB uncompressed (freed after each iteration)
    // Total queue memory: ~60MB (trade-off: memory vs throughput)
    const size_t QUEUE_CAPACITY = 4;
    BoundedBlockingQueue<InputItem> input_queue(QUEUE_CAPACITY);
    BoundedBlockingQueue<OutputItem> output_queue(QUEUE_CAPACITY);
    PipelineMetrics metrics;

    // Error flag for graceful shutdown
    std::atomic<bool> error_flag(false);

    logger::info("Pipeline queues created: input_queue(cap=" + std::to_string(QUEUE_CAPACITY) +
                "), output_queue(cap=" + std::to_string(QUEUE_CAPACITY) + ")");

    // Launch 3 threads in parallel
    // Thread 1: Reader (stdin → input_queue)
    std::thread reader_thread(
        reader_thread_func,
        std::ref(input_queue),
        num_images,
        std::ref(error_flag)
    );

    // Thread 2: Worker (input_queue → GPU → output_queue)
    const bool log_memory = opts.verbose || opts.profiling;
    std::thread worker_thread(
        worker_thread_func,
        std::ref(input_queue),
        std::ref(output_queue),
        engine,
        opts.output_format,
        std::ref(error_flag),
        std::ref(metrics),
        log_memory
    );

    // Thread 3: Writer (output_queue → stdout)
    std::thread writer_thread(
        writer_thread_func,
        std::ref(output_queue),
        std::ref(error_flag)
    );

    logger::info("Pipeline threads launched: reader, worker, writer running in parallel");

    // Wait for all threads to complete (join)
    reader_thread.join();
    logger::info("Reader thread joined");

    worker_thread.join();
    logger::info("Worker thread joined");

    writer_thread.join();
    logger::info("Writer thread joined");

    // Check for errors
    log_pipeline_metrics(metrics);

    if (error_flag) {
        logger::error("Batch processing failed: error occurred in one or more threads");
        return 1;
    }

    logger::info("Batch stdin mode completed successfully (pipeline v4)");
    return 0;
}
