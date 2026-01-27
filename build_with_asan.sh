#!/bin/bash
# Build NCNN upscaler with AddressSanitizer for memory leak detection
# Usage: ./build_with_asan.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build_asan"

echo "🔍 Building with AddressSanitizer (ASan) for memory leak detection..."
echo "Build directory: $BUILD_DIR"

# Create clean build directory
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Configure with ASan flags
echo "📦 Configuring CMake with ASan..."
cmake ../bdreader-ncnn-upscaler \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g -O1" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"

# Build
echo "🔨 Building..."
cmake --build . -j$(nproc)

echo "✅ Build complete!"
echo ""
echo "Binary location: $BUILD_DIR/bdreader-ncnn-upscaler"
echo ""
echo "Usage:"
echo "  export ASAN_OPTIONS='detect_leaks=1:log_path=asan_report.txt'"
echo "  ./build_asan/bdreader-ncnn-upscaler --engine realcugan --mode stdin --batch-size 10 ..."
echo ""
echo "ASan will report memory leaks and errors to asan_report.txt.*"
