#!/bin/bash
# Test NCNN with AddressSanitizer to detect memory leaks
# Usage: ./test_asan_memory.sh [num_images] [test_image]

set -e

NUM_IMAGES="${1:-5}"
TEST_IMAGE="${2:-img_test/P00003.jpg}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINARY="$SCRIPT_DIR/build_asan/bdreader-ncnn-upscaler"

if [ ! -f "$BINARY" ]; then
    echo "❌ Error: ASan binary not found at $BINARY"
    echo "Please run ./build_with_asan.sh first"
    exit 1
fi

if [ ! -f "$TEST_IMAGE" ]; then
    echo "❌ Error: Test image not found: $TEST_IMAGE"
    exit 1
fi

echo "(Note: the test image argument is kept for compatibility but the current keep-alive run uses built-in PNG payloads.)"

# Configure AddressSanitizer
export ASAN_OPTIONS='detect_leaks=1:log_path=asan_report.txt:halt_on_error=0:print_stats=1'

echo "🧪 Testing NCNN with AddressSanitizer"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "Binary: $BINARY"
echo "Test image: $TEST_IMAGE"
echo "Batch size: $NUM_IMAGES images"
echo "ASan options: $ASAN_OPTIONS"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

# Run test with ASan
echo "🚀 Running keep-alive iterations..."
for ((i=1; i<=NUM_IMAGES; i++)); do
    echo "  ➤ Iteration $i/$NUM_IMAGES"
    python3 "$SCRIPT_DIR/protocol_v2_integration.py" --binary "$BINARY" --gpu-id 0 >/dev/null
done

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "✅ Test completed!"
echo ""

# Check for ASan reports
if ls asan_report.txt.* 1> /dev/null 2>&1; then
    echo "📊 AddressSanitizer reports found:"
    ls -lh asan_report.txt.*
    echo ""
    echo "📝 Summary of latest report:"
    LATEST_REPORT=$(ls -t asan_report.txt.* | head -1)
    echo "File: $LATEST_REPORT"
    echo ""

    # Show memory leak summary
    if grep -q "SUMMARY: AddressSanitizer" "$LATEST_REPORT"; then
        echo "⚠️  Memory issues detected:"
        grep "SUMMARY: AddressSanitizer" "$LATEST_REPORT"
        echo ""
        echo "Full report available in: $LATEST_REPORT"
    else
        echo "✅ No memory leaks detected!"
    fi
else
    echo "✅ No ASan reports generated (no memory issues detected)"
fi

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
