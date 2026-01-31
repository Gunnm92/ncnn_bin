#!/usr/bin/env python3
"""Simple test direct du binaire GPU avec vraies images"""
import argparse
import pathlib
import struct
import subprocess
import sys
import time
from datetime import datetime

K_MAGIC = 0x42524452
K_VERSION = 2
MSG_TYPE_REQUEST = 1


def build_request(request_id, images):
    header = struct.pack("<IIII", K_MAGIC, K_VERSION, MSG_TYPE_REQUEST, request_id)
    payload = bytearray()
    payload.append(0)  # engine realcugan
    payload.extend(struct.pack("<I", 1))
    payload.extend(b"E")
    payload.extend(struct.pack("<i", 0))  # gpu_id
    payload.extend(struct.pack("<I", len(images)))
    for img in images:
        payload.extend(struct.pack("<I", len(img)))
        payload.extend(img)
    return struct.pack("<I", len(header) + len(payload)) + header + payload


def parse_response(data):
    req_id, status, err_len = struct.unpack_from("<III", data, 0)
    offset = 12
    error = data[offset:offset+err_len].decode("utf-8") if err_len > 0 else ""
    offset += err_len
    result_count = struct.unpack_from("<I", data, offset)[0]
    offset += 4
    outputs = []
    for _ in range(result_count):
        out_len = struct.unpack_from("<I", data, offset)[0]
        offset += 4
        outputs.append(data[offset:offset+out_len])
        offset += out_len
    return req_id, status, error, outputs


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default="bdreader-ncnn-upscaler/build-release/bdreader-ncnn-upscaler")
    parser.add_argument("--input-dir", default="tests_input")
    parser.add_argument("--output-dir", default=f"test_output_{datetime.now().strftime('%Y-%m-%d')}")
    parser.add_argument("--gpu-id", default="0")
    parser.add_argument("--num-images", type=int, default=5)
    args = parser.parse_args()

    # Create output directory
    pathlib.Path(args.output_dir).mkdir(exist_ok=True)
    print(f"Output directory: {args.output_dir}/")

    # Load images
    print(f"\nLoading images from {args.input_dir}...")
    images = []
    for img_path in sorted(pathlib.Path(args.input_dir).glob("*.jpg"))[:args.num_images]:
        with open(img_path, 'rb') as f:
            data = f.read()
            images.append((img_path.name, data))
            print(f"  Loaded: {img_path.name} ({len(data)/1024:.1f} KB)")

    if not images:
        print("ERROR: No images found!")
        return 1

    print(f"\nStarting keep-alive process...")
    cmd = [args.binary, "--mode", "stdin", "--keep-alive", "--gpu-id", args.gpu_id, "--verbose"]
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    time.sleep(1)
    print("Process started\n")

    # Process each image
    for idx, (name, img_data) in enumerate(images, 1):
        print(f"[{idx}/{len(images)}] Processing {name}...")

        frame = build_request(idx, [img_data])

        start = time.time()
        proc.stdin.write(frame)
        proc.stdin.flush()

        # Read response
        resp_len_bytes = proc.stdout.read(4)
        if len(resp_len_bytes) < 4:
            print(f"  ❌ No response")
            break

        resp_len = struct.unpack("<I", resp_len_bytes)[0]
        payload = proc.stdout.read(resp_len)
        elapsed = time.time() - start

        req_id, status, error, outputs = parse_response(payload)

        if status == 0 and len(outputs) == 1:
            # Save output
            output_name = name.replace('.jpg', '.webp')
            output_path = pathlib.Path(args.output_dir) / output_name
            with open(output_path, 'wb') as f:
                f.write(outputs[0])

            print(f"  ✅ SUCCESS: {len(outputs[0])/1024:.1f} KB, {elapsed:.2f}s → {output_path}")
        else:
            print(f"  ❌ FAILED: status={status}, error={error}")

    # Shutdown
    proc.stdin.write(struct.pack("<I", 0))
    proc.stdin.close()
    proc.wait(timeout=10)

    print(f"\n✅ Done! Output saved to {args.output_dir}/")
    print(f"\nTo view results:")
    print(f"  ls -lh {args.output_dir}/")

    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nInterrupted")
        sys.exit(1)
    except Exception as e:
        print(f"ERROR: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
