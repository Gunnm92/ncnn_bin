#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <webp/decode.h>
#include <webp/encode.h>
#include <webp/mux.h>
#include <webp/mux_types.h>
#include <webp/types.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "utils/image_io.hpp"

namespace {
void write_callback(void* user, void* data, int size) {
    auto* buffer = static_cast<std::vector<uint8_t>*>(user);
    const auto* src = static_cast<const uint8_t*>(data);
    buffer->insert(buffer->end(), src, src + size);
}

// RAII wrapper for STB image data
struct STBImageRAII {
    stbi_uc* pixels = nullptr;
    
    STBImageRAII() = default;
    
    ~STBImageRAII() {
        if (pixels) {
            stbi_image_free(pixels);
            pixels = nullptr;
        }
    }
    
    // Non-copyable
    STBImageRAII(const STBImageRAII&) = delete;
    STBImageRAII& operator=(const STBImageRAII&) = delete;
    
    // Movable
    STBImageRAII(STBImageRAII&& other) noexcept : pixels(other.pixels) {
        other.pixels = nullptr;
    }
    
    STBImageRAII& operator=(STBImageRAII&& other) noexcept {
        if (this != &other) {
            if (pixels) {
                stbi_image_free(pixels);
            }
            pixels = other.pixels;
            other.pixels = nullptr;
        }
        return *this;
    }
    
    stbi_uc* get() { return pixels; }
    void reset(stbi_uc* p) { 
        if (pixels) {
            stbi_image_free(pixels);
        }
        pixels = p;
    }
};

// RAII wrapper for WebPMemoryWriter
struct WebPMemoryWriterRAII {
    WebPMemoryWriter writer;
    
    WebPMemoryWriterRAII() {
        WebPMemoryWriterInit(&writer);
    }
    
    ~WebPMemoryWriterRAII() {
        WebPMemoryWriterClear(&writer);
    }
    
    // Non-copyable
    WebPMemoryWriterRAII(const WebPMemoryWriterRAII&) = delete;
    WebPMemoryWriterRAII& operator=(const WebPMemoryWriterRAII&) = delete;
    
    // Movable
    WebPMemoryWriterRAII(WebPMemoryWriterRAII&&) = default;
    WebPMemoryWriterRAII& operator=(WebPMemoryWriterRAII&&) = default;
    
    WebPMemoryWriter* get() { return &writer; }
};

// RAII wrapper for WebPPicture
struct WebPPictureRAII {
    WebPPicture pic;
    bool initialized = false;
    
    WebPPictureRAII() {
        initialized = WebPPictureInit(&pic) != 0;
    }
    
    ~WebPPictureRAII() {
        if (initialized) {
            WebPPictureFree(&pic);
        }
    }
    
    // Non-copyable, non-movable (WebPPicture contains pointers)
    WebPPictureRAII(const WebPPictureRAII&) = delete;
    WebPPictureRAII& operator=(const WebPPictureRAII&) = delete;
    WebPPictureRAII(WebPPictureRAII&&) = delete;
    WebPPictureRAII& operator=(WebPPictureRAII&&) = delete;
    
    WebPPicture* get() { return initialized ? &pic : nullptr; }
    bool is_initialized() const { return initialized; }
};
} // namespace

namespace image_io {

bool decode_image(const uint8_t* data, size_t size, ImagePixels& out) {
    if (!data || size == 0) {
        return false;
    }
    int width, height, channels;
    
    // Use RAII wrapper - automatically freed even if exception occurs
    STBImageRAII pixels_raii;
    pixels_raii.reset(stbi_load_from_memory(
        data, 
        static_cast<int>(size), 
        &width, 
        &height, 
        &channels, 
        3
    ));
    
    if (!pixels_raii.get()) {
        return false;
    }
    
    out.width = width;
    out.height = height;
    out.channels = 3;
    out.pixels.assign(pixels_raii.get(), pixels_raii.get() + width * height * 3);
    
    // RAII destructor automatically calls stbi_image_free()
    return true;
}

bool encode_image(const ImagePixels& img, const std::string& format, std::vector<uint8_t>& out) {
    out.clear();
    const int quality = 90;
    std::string fmt = format.empty() ? "webp" : format;
    std::transform(fmt.begin(), fmt.end(), fmt.begin(), [](unsigned char c) { return std::tolower(c); });

    if (fmt == "webp") {
        WebPConfig config;
        if (!WebPConfigInit(&config)) {
            return false;
        }
        config.quality = quality;

        // Use RAII wrappers - automatically cleaned up even if exception occurs
        WebPPictureRAII pic_raii;
        if (!pic_raii.is_initialized()) {
            return false;
        }
        
        WebPPicture* pic = pic_raii.get();
        pic->width = img.width;
        pic->height = img.height;

        WebPMemoryWriterRAII writer_raii;
        pic->writer = WebPMemoryWrite;
        pic->custom_ptr = writer_raii.get();

        if (!WebPPictureImportRGB(pic, img.pixels.data(), img.width * img.channels)) {
            return false;
        }

        const bool ok = WebPEncode(&config, pic) != 0;
        if (ok) {
            WebPMemoryWriter* writer = writer_raii.get();
            out.assign(writer->mem, writer->mem + writer->size);
        }
        
        // RAII destructors automatically call WebPMemoryWriterClear and WebPPictureFree
        return ok;
    }

    if (fmt == "png") {
        return stbi_write_png_to_func(write_callback, &out, img.width, img.height, img.channels, img.pixels.data(), img.width * img.channels) != 0;
    }

    if (fmt == "jpg" || fmt == "jpeg") {
        return stbi_write_jpg_to_func(write_callback, &out, img.width, img.height, img.channels, img.pixels.data(), quality) != 0;
    }

    return false;
}

} // namespace image_io
