#!/usr/bin/env python3
"""
Test batch stdin protocol with NCNN binary

Protocol v1:
  Input:  [num_images:u32][size1:u32][data1][size2:u32][data2]...
  Output: [num_results:u32][size1:u32][data1][size2:u32][data2]...
"""

import struct
import subprocess
import sys
from pathlib import Path

def write_u32(value):
    """Write uint32 in little-endian"""
    return struct.pack('<I', value)

def read_u32(data, offset):
    """Read uint32 from little-endian bytes"""
    return struct.unpack_from('<I', data, offset)[0]

def test_batch_stdin():
    # Load test images
    img_dir = Path("img_test")
    test_images = [
        img_dir / "P00003.jpg",
        img_dir / "P00004.jpg",
        img_dir / "P00005.jpg",
    ]

    image_data = []
    for img_path in test_images:
        if not img_path.exists():
            print(f"Error: Test image not found: {img_path}")
            return False
        with open(img_path, 'rb') as f:
            image_data.append(f.read())

    print(f"Loaded {len(image_data)} test images")
    for i, data in enumerate(image_data):
        print(f"  Image {i}: {len(data)} bytes")

    # Build stdin payload according to protocol v1
    stdin_payload = bytearray()

    # Write number of images
    stdin_payload.extend(write_u32(len(image_data)))

    # Write each image
    for data in image_data:
        stdin_payload.extend(write_u32(len(data)))
        stdin_payload.extend(data)

    print(f"\nStdin payload: {len(stdin_payload)} bytes total")

    # Run NCNN binary with batch stdin mode
    cmd = [
        "./build/bdreader-ncnn-upscaler",
        "--engine", "realcugan",
        "--mode", "stdin",
        "--batch-size", str(len(image_data)),
        "--quality", "F",
        "--model", "/config/workspace/BDReader-Rust/backend/models/realcugan/models-se",
        "--gpu-id", "0",
    ]

    print(f"\nRunning command: {' '.join(cmd)}")

    try:
        proc = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        stdout, stderr = proc.communicate(input=bytes(stdin_payload), timeout=30)

        if proc.returncode != 0:
            print(f"Error: Process returned {proc.returncode}")
            print(f"Stderr: {stderr.decode('utf-8', errors='replace')}")
            return False

        print(f"Success! Got {len(stdout)} bytes output")

        # Parse output according to protocol v1
        if len(stdout) < 4:
            print("Error: Output too short")
            return False

        num_results = read_u32(stdout, 0)
        print(f"Number of results: {num_results}")

        if num_results != len(image_data):
            print(f"Error: Expected {len(image_data)} results, got {num_results}")
            return False

        # Parse each result
        offset = 4
        for i in range(num_results):
            if offset + 4 > len(stdout):
                print(f"Error: Unexpected end of data at result {i}")
                return False

            result_size = read_u32(stdout, offset)
            offset += 4

            if offset + result_size > len(stdout):
                print(f"Error: Result {i} size mismatch")
                return False

            result_data = stdout[offset:offset + result_size]
            offset += result_size

            print(f"  Result {i}: {result_size} bytes")

            # Save result
            output_path = Path("results") / f"batch_stdin_test_{i}.webp"
            output_path.parent.mkdir(exist_ok=True)
            with open(output_path, 'wb') as f:
                f.write(result_data)
            print(f"    Saved to: {output_path}")

        print(f"\n✅ Batch stdin test passed!")
        print(f"   Processed {num_results} images successfully")
        return True

    except subprocess.TimeoutExpired:
        print("Error: Process timed out")
        proc.kill()
        return False
    except Exception as e:
        print(f"Error: {e}")
        return False

if __name__ == "__main__":
    success = test_batch_stdin()
    sys.exit(0 if success else 1)
