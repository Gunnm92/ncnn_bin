#!/bin/bash

# Performance test for NCNN upscalers
# Tests different models and measures execution time

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
INPUT_IMG="$ROOT_DIR/img_test/P00003.jpg"
OUTPUT_DIR="$ROOT_DIR/results/perf_test"
MODELS_PATH="$ROOT_DIR/models"

mkdir -p "$OUTPUT_DIR"

echo "==================================="
echo "NCNN Upscaler Performance Test"
echo "==================================="
echo "Input: $INPUT_IMG (900x1221)"
echo "GPU: NVIDIA RTX 3090"
echo ""

# Test RealCUGAN models
echo "--- RealCUGAN Models (2x) ---"
for quality in F E Q H; do
    echo -n "Quality $quality: "
    START=$(date +%s.%N)
    ./build/bdreader-ncnn-upscaler \
        --engine realcugan \
        --mode file \
        --input "$INPUT_IMG" \
        --output "$OUTPUT_DIR/realcugan_$quality.webp" \
        --gpu-id 0 \
        --quality $quality \
        --model "$MODELS_PATH/realcugan/models-se" \
        2>&1 | grep -q "File mode completed"
    END=$(date +%s.%N)
    DURATION=$(echo "$END - $START" | bc)

    # Check output
    if [ -f "$OUTPUT_DIR/realcugan_$quality.webp" ]; then
        SIZE=$(stat -c%s "$OUTPUT_DIR/realcugan_$quality.webp")
        SIZE_KB=$((SIZE / 1024))
        MEAN=$(python3 -c "from PIL import Image; import numpy as np; img = Image.open('$OUTPUT_DIR/realcugan_$quality.webp'); arr = np.array(img); print(f'{arr.mean():.1f}')")
        echo "${DURATION}s | ${SIZE_KB}KB | Mean=$MEAN"
    else
        echo "FAILED"
    fi
done

echo ""
echo "--- RealESRGAN animevideov3 Models ---"
for scale in 2 4; do
    echo -n "Scale ${scale}x: "
    START=$(date +%s.%N)
    ./build/bdreader-ncnn-upscaler \
        --engine realesrgan \
        --mode file \
        --input "$INPUT_IMG" \
        --output "$OUTPUT_DIR/realesrgan_animevideov3_x${scale}.webp" \
        --gpu-id 0 \
        --scale $scale \
        --model "$MODELS_PATH/realesrgan" \
        2>&1 | grep -q "File mode completed"
    END=$(date +%s.%N)
    DURATION=$(echo "$END - $START" | bc)

    if [ -f "$OUTPUT_DIR/realesrgan_animevideov3_x${scale}.webp" ]; then
        SIZE=$(stat -c%s "$OUTPUT_DIR/realesrgan_animevideov3_x${scale}.webp")
        SIZE_KB=$((SIZE / 1024))
        MEAN=$(python3 -c "from PIL import Image; import numpy as np; img = Image.open('$OUTPUT_DIR/realesrgan_animevideov3_x${scale}.webp'); arr = np.array(img); print(f'{arr.mean():.1f}')")
        echo "${DURATION}s | ${SIZE_KB}KB | Mean=$MEAN"
    else
        echo "FAILED"
    fi
done

echo ""
echo "--- RealESRGAN x4plus-anime ---"
echo -n "x4plus-anime: "
START=$(date +%s.%N)
./build/bdreader-ncnn-upscaler \
    --engine realesrgan \
    --mode file \
    --input "$INPUT_IMG" \
    --output "$OUTPUT_DIR/realesrgan_x4plus_anime.webp" \
    --gpu-id 0 \
    --model "$MODELS_PATH/realesrgan" \
    --model-name realesrgan-x4plus-anime \
    2>&1 | grep -q "File mode completed"
END=$(date +%s.%N)
DURATION=$(echo "$END - $START" | bc)

if [ -f "$OUTPUT_DIR/realesrgan_x4plus_anime.webp" ]; then
    SIZE=$(stat -c%s "$OUTPUT_DIR/realesrgan_x4plus_anime.webp")
    SIZE_KB=$((SIZE / 1024))
    MEAN=$(python3 -c "from PIL import Image; import numpy as np; img = Image.open('$OUTPUT_DIR/realesrgan_x4plus_anime.webp'); arr = np.array(img); print(f'{arr.mean():.1f}')")
    echo "${DURATION}s | ${SIZE_KB}KB | Mean=$MEAN"
else
    echo "FAILED"
fi

echo ""
echo "--- Official Binary Comparison ---"
echo -n "Official x4plus-anime: "
START=$(date +%s.%N)
/config/workspace/BDReader-Rust/backend/bin/realesrgan-ncnn-vulkan \
    -i "$INPUT_IMG" \
    -o "$OUTPUT_DIR/official_x4plus_anime.webp" \
    -n realesrgan-x4plus-anime \
    -s 4 \
    -m "$MODELS_PATH/realesrgan" \
    2>&1 > /dev/null
END=$(date +%s.%N)
DURATION=$(echo "$END - $START" | bc)

if [ -f "$OUTPUT_DIR/official_x4plus_anime.webp" ]; then
    SIZE=$(stat -c%s "$OUTPUT_DIR/official_x4plus_anime.webp")
    SIZE_KB=$((SIZE / 1024))
    MEAN=$(python3 -c "from PIL import Image; import numpy as np; img = Image.open('$OUTPUT_DIR/official_x4plus_anime.webp'); arr = np.array(img); print(f'{arr.mean():.1f}')")
    echo "${DURATION}s | ${SIZE_KB}KB | Mean=$MEAN"
else
    echo "FAILED"
fi

echo ""
echo "==================================="
echo "Performance test complete!"
echo "Results saved in: $OUTPUT_DIR"
echo "==================================="
