#!/usr/bin/env python3
"""
Test NCNN batch processing with debug logging
Usage: python3 test_batch_debug.py [num_images] [test_image_path]
"""

import sys
import struct
import subprocess
from pathlib import Path

def create_batch_payload(image_path: Path, num_images: int) -> bytes:
    """Create Protocol v4 binary payload for batch processing"""

    # Read test image
    with open(image_path, 'rb') as f:
        image_data = f.read()

    image_size = len(image_data)
    print(f"Image: {image_path}")
    print(f"Image size: {image_size:,} bytes ({image_size / 1024 / 1024:.2f} MB)")

    # Build payload: [num_images:u32][size1:u32][data1][size2:u32][data2]...
    payload = bytearray()

    # Write num_images (u32 little-endian)
    payload.extend(struct.pack('<I', num_images))

    # Write each image
    for i in range(num_images):
        # Write image size (u32 little-endian)
        payload.extend(struct.pack('<I', image_size))
        # Write image data
        payload.extend(image_data)

    total_size = len(payload)
    expected_size = 4 + num_images * (4 + image_size)

    print(f"\nPayload built:")
    print(f"  Total size: {total_size:,} bytes ({total_size / 1024 / 1024:.2f} MB)")
    print(f"  Expected: {expected_size:,} bytes")
    print(f"  Match: {'✅' if total_size == expected_size else '❌'}")

    return bytes(payload)

def run_ncnn_binary(payload: bytes, binary_path: Path, config: dict):
    """Run NCNN binary with stdin payload and capture output"""

    # Create output directory
    output_dir = Path("/tmp/ncnn_test_debug")
    output_dir.mkdir(exist_ok=True)

    cmd = [
        str(binary_path),
        '--engine', config['engine'],
        '--mode', 'stdin',
        '--batch-size', str(config['num_images']),
        '--quality', config['quality'],
        '--model', config['model_path'],
        '--gpu-id', str(config['gpu_id']),
    ]

    print(f"\n{'='*60}")
    print("Launching NCNN binary...")
    print(f"{'='*60}")
    print(f"Command: {' '.join(cmd)}")
    print(f"Stdin payload: {len(payload):,} bytes")
    print(f"Output dir: {output_dir}")
    print(f"{'='*60}\n")

    # Save payload to file
    payload_file = output_dir / "payload.bin"
    with open(payload_file, 'wb') as f:
        f.write(payload)
    print(f"Payload saved to: {payload_file}")

    try:
        # Run binary with stdin payload
        result = subprocess.run(
            cmd,
            input=payload,
            capture_output=True,
            timeout=120  # 2 minute timeout
        )

        # Decode outputs
        stdout = result.stdout
        stderr = result.stderr.decode('utf-8', errors='replace')

        # Save outputs to files
        stdout_file = output_dir / "stdout.bin"
        stderr_file = output_dir / "stderr.log"

        with open(stdout_file, 'wb') as f:
            f.write(stdout)
        print(f"Stdout saved to: {stdout_file} ({len(stdout):,} bytes)")

        with open(stderr_file, 'w') as f:
            f.write(stderr)
        print(f"Stderr saved to: {stderr_file} ({len(stderr):,} bytes)")

        print("\nSTDERR (Debug Log):")
        print(stderr)

        print(f"\n{'='*60}")
        print(f"Binary exit code: {result.returncode}")
        print(f"Stdout size: {len(stdout):,} bytes")
        print(f"Stderr size: {len(stderr):,} bytes")
        print(f"{'='*60}")

        if result.returncode == 0:
            print("✅ Test PASSED")

            # Parse output (Protocol v4)
            if len(stdout) >= 4:
                num_results = struct.unpack('<I', stdout[:4])[0]
                print(f"\nResults: {num_results} images returned")

                # Parse each result size
                offset = 4
                for i in range(num_results):
                    if offset + 4 <= len(stdout):
                        result_size = struct.unpack('<I', stdout[offset:offset+4])[0]
                        print(f"  Image {i}: {result_size:,} bytes ({result_size / 1024 / 1024:.2f} MB)")
                        offset += 4 + result_size

        else:
            print("❌ Test FAILED")
            print("\nLast 50 lines of stderr:")
            stderr_lines = stderr.strip().split('\n')
            for line in stderr_lines[-50:]:
                print(f"  {line}")

        return result.returncode

    except subprocess.TimeoutExpired:
        print("❌ TIMEOUT: Binary took longer than 120 seconds")
        return -1
    except Exception as e:
        print(f"❌ ERROR: {e}")
        import traceback
        traceback.print_exc()
        return -1

def main():
    # Parse arguments
    num_images = int(sys.argv[1]) if len(sys.argv) > 1 else 3
    test_image = Path(sys.argv[2]) if len(sys.argv) > 2 else Path("/config/workspace/BDReader-Rust/ncnn_bin/img_test/P00003.jpg")

    # Config
    config = {
        'binary_path': Path("/config/workspace/BDReader-Rust/ncnn_bin/build/bdreader-ncnn-upscaler"),
        'num_images': num_images,
        'engine': 'realcugan',
        'quality': 'F',  # Fast mode for testing
        'model_path': '/config/workspace/BDReader-Rust/backend/models/realcugan/models-se',
        'gpu_id': 0,
    }

    print("="*60)
    print("NCNN Batch Processing Debug Test")
    print("="*60)
    print(f"Num images: {config['num_images']}")
    print(f"Test image: {test_image}")
    print(f"Binary: {config['binary_path']}")
    print(f"Engine: {config['engine']}")
    print(f"Quality: {config['quality']}")
    print(f"Model: {config['model_path']}")
    print("="*60)

    # Verify files exist
    if not test_image.exists():
        print(f"❌ ERROR: Test image not found: {test_image}")
        return 1

    if not config['binary_path'].exists():
        print(f"❌ ERROR: Binary not found: {config['binary_path']}")
        return 1

    # Create payload
    payload = create_batch_payload(test_image, config['num_images'])

    # Run binary
    exit_code = run_ncnn_binary(payload, config['binary_path'], config)

    return exit_code

if __name__ == '__main__':
    sys.exit(main())
