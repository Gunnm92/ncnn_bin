#!/bin/bash
set -e

# Test NCNN batch processing with debug logging
# Usage: ./test_batch_debug.sh <num_images> <test_image_path>

NUM_IMAGES=${1:-3}
TEST_IMAGE=${2:-"/config/workspace/BDReader-Rust/ncnn_bin/img_test/P00003.jpg"}

BINARY="/config/workspace/BDReader-Rust/ncnn_bin/build/bdreader-ncnn-upscaler"
ENGINE="realcugan"
QUALITY="F"
MODEL_PATH="/config/workspace/BDReader-Rust/backend/models/realcugan/models-se"
OUTPUT_DIR="/tmp/ncnn_test_output"

mkdir -p "$OUTPUT_DIR"

echo "=========================================="
echo "NCNN Batch Processing Debug Test"
echo "=========================================="
echo "Binary: $BINARY"
echo "Test image: $TEST_IMAGE"
echo "Num images: $NUM_IMAGES"
echo "Engine: $ENGINE"
echo "Quality: $QUALITY"
echo "Model: $MODEL_PATH"
echo "=========================================="

if [ ! -f "$TEST_IMAGE" ]; then
    echo "ERROR: Test image not found: $TEST_IMAGE"
    exit 1
fi

if [ ! -f "$BINARY" ]; then
    echo "ERROR: Binary not found: $BINARY"
    exit 1
fi

# Create stdin payload (Protocol v4)
# Format: [num_images:u32][size1:u32][data1][size2:u32][data2]...

echo "Building stdin payload..."

# Read test image into variable
IMAGE_DATA=$(cat "$TEST_IMAGE")
IMAGE_SIZE=$(stat -f%z "$TEST_IMAGE" 2>/dev/null || stat -c%s "$TEST_IMAGE")

echo "Image size: $IMAGE_SIZE bytes"

# Create payload file
PAYLOAD="/tmp/ncnn_batch_payload.bin"
rm -f "$PAYLOAD"

# Write num_images (u32 little-endian)
printf "$(printf '%08x' $NUM_IMAGES | sed 's/\(..\)\(..\)\(..\)\(..\)/\\x\4\\x\3\\x\2\\x\1/')" > "$PAYLOAD"

# Write each image
for i in $(seq 1 $NUM_IMAGES); do
    # Write image size (u32 little-endian)
    printf "$(printf '%08x' $IMAGE_SIZE | sed 's/\(..\)\(..\)\(..\)\(..\)/\\x\4\\x\3\\x\2\\x\1/')" >> "$PAYLOAD"
    # Write image data
    cat "$TEST_IMAGE" >> "$PAYLOAD"
done

PAYLOAD_SIZE=$(stat -f%z "$PAYLOAD" 2>/dev/null || stat -c%s "$PAYLOAD")
echo "Payload size: $PAYLOAD_SIZE bytes"
echo "Expected: 4 + ($NUM_IMAGES × (4 + $IMAGE_SIZE)) = 4 + ($NUM_IMAGES × $((4 + IMAGE_SIZE))) = $((4 + NUM_IMAGES * (4 + IMAGE_SIZE))) bytes"

echo ""
echo "Launching NCNN binary with DEBUG logging..."
echo "=========================================="

# Run binary with stdin payload and capture output
# Enable debug logging via stderr
cat "$PAYLOAD" | \
    "$BINARY" \
    --engine "$ENGINE" \
    --mode stdin \
    --batch-size $NUM_IMAGES \
    --quality "$QUALITY" \
    --model "$MODEL_PATH" \
    --gpu-id 0 \
    2>&1 | tee "$OUTPUT_DIR/debug.log"

EXIT_CODE=${PIPESTATUS[1]}

echo ""
echo "=========================================="
echo "Binary exit code: $EXIT_CODE"
echo "Debug log saved to: $OUTPUT_DIR/debug.log"
echo "=========================================="

if [ $EXIT_CODE -eq 0 ]; then
    echo "✅ Test PASSED"
else
    echo "❌ Test FAILED"
    echo ""
    echo "Last 50 lines of debug log:"
    tail -50 "$OUTPUT_DIR/debug.log"
fi

exit $EXIT_CODE
