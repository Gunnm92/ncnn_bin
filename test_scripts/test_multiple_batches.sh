#!/bin/bash
# Test multiple successive batches to detect memory leaks
# Usage: ./test_multiple_batches.sh [num_batches] [batch_size] [test_image]

set -e

NUM_BATCHES="${1:-5}"
BATCH_SIZE="${2:-5}"
TEST_IMAGE="${3:-img_test/P00003.jpg}"

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

# Configure AddressSanitizer
export ASAN_OPTIONS='detect_leaks=1:log_path=asan_report.txt:halt_on_error=0:print_stats=1'

echo "🧪 Testing NCNN with Multiple Successive Batches"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "Binary: $BINARY"
echo "Test image: $TEST_IMAGE"
echo "Number of batches: $NUM_BATCHES"
echo "Batch size: $BATCH_SIZE images"
echo "Total images to process: $((NUM_BATCHES * BATCH_SIZE))"
echo "ASan options: $ASAN_OPTIONS"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

# Clean up old ASan reports
rm -f asan_report.txt.*

# Run multiple batches
TOTAL_TIME=0
for ((i=1; i<=NUM_BATCHES; i++)); do
    echo "🚀 Running batch $i/$NUM_BATCHES..."

    START_TIME=$(date +%s%3N)
    python3 "$SCRIPT_DIR/test_batch_debug.py" "$BATCH_SIZE" "$TEST_IMAGE" --binary "$BINARY" > /dev/null 2>&1
    END_TIME=$(date +%s%3N)

    ELAPSED=$((END_TIME - START_TIME))
    TOTAL_TIME=$((TOTAL_TIME + ELAPSED))

    echo "  ✅ Batch $i completed in ${ELAPSED}ms"

    # Check for ASan reports after each batch
    if ls asan_report.txt.* 1> /dev/null 2>&1; then
        LATEST_REPORT=$(ls -t asan_report.txt.* | head -1)
        if grep -q "SUMMARY: AddressSanitizer" "$LATEST_REPORT"; then
            echo "  ⚠️  Memory issues detected in batch $i!"
            echo "  Report: $LATEST_REPORT"
        fi
    fi

    # Small delay between batches to allow cleanup
    sleep 0.5
done

AVG_TIME=$((TOTAL_TIME / NUM_BATCHES))

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "✅ All batches completed!"
echo ""
echo "📊 Performance Summary:"
echo "  Total batches: $NUM_BATCHES"
echo "  Total images: $((NUM_BATCHES * BATCH_SIZE))"
echo "  Total time: ${TOTAL_TIME}ms"
echo "  Average time per batch: ${AVG_TIME}ms"
echo "  Average time per image: $((AVG_TIME / BATCH_SIZE))ms"
echo ""

# Check for ASan reports
if ls asan_report.txt.* 1> /dev/null 2>&1; then
    echo "📊 AddressSanitizer reports found:"
    ls -lh asan_report.txt.*
    echo ""
    echo "📝 Analysis of reports:"

    # Count total leak reports
    LEAK_COUNT=0
    for report in asan_report.txt.*; do
        if grep -q "SUMMARY: AddressSanitizer" "$report"; then
            LEAK_COUNT=$((LEAK_COUNT + 1))
        fi
    done

    if [ $LEAK_COUNT -gt 0 ]; then
        echo "⚠️  Memory issues detected in $LEAK_COUNT report(s)"
        echo ""
        echo "Latest report:"
        LATEST_REPORT=$(ls -t asan_report.txt.* | head -1)
        echo "File: $LATEST_REPORT"
        echo ""
        grep "SUMMARY: AddressSanitizer" "$LATEST_REPORT" || true
        echo ""
        echo "⚠️  MEMORY LEAK DETECTED - Review reports for details"
    else
        echo "✅ No memory leaks detected across all batches!"
    fi
else
    echo "✅ No ASan reports generated (no memory issues detected)"
fi

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
