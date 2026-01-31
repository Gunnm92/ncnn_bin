#!/usr/bin/env python3
"""Test with real images from tests_input/"""
import argparse
import pathlib
import psutil
import struct
import subprocess
import sys
import time

K_MAGIC = 0x42524452
K_VERSION = 2
MSG_TYPE_REQUEST = 1


def build_request(request_id, engine, meta, gpu_id, images):
    header = struct.pack("<IIII", K_MAGIC, K_VERSION, MSG_TYPE_REQUEST, request_id)
    payload = bytearray()
    payload.append(engine)
    payload.extend(struct.pack("<I", len(meta)))
    payload.extend(meta.encode())
    payload.extend(struct.pack("<i", gpu_id))
    payload.extend(struct.pack("<I", len(images)))
    for img in images:
        payload.extend(struct.pack("<I", len(img)))
        payload.extend(img)
    return struct.pack("<I", len(header) + len(payload)) + header + payload


def parse_response(data):
    offset = 0
    request_id, status, error_len = struct.unpack_from("<III", data, offset)
    offset += 12
    error = data[offset : offset + error_len].decode("utf-8", "replace")
    offset += error_len
    result_count = struct.unpack_from("<I", data, offset)[0]
    offset += 4
    outputs = []
    for _ in range(result_count):
        out_len = struct.unpack_from("<I", data, offset)[0]
        offset += 4
        outputs.append(data[offset : offset + out_len])
        offset += out_len
    return request_id, status, error, outputs


def load_images(input_dir, pattern="*.jpg", limit=10):
    """Load real test images"""
    images = []
    for img_path in sorted(pathlib.Path(input_dir).glob(pattern))[:limit]:
        with open(img_path, 'rb') as f:
            data = f.read()
            images.append((img_path.name, data))
            print(f"  Loaded: {img_path.name} ({len(data)/1024:.1f} KB)")
    return images


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default="bdreader-ncnn-upscaler/build-release/bdreader-ncnn-upscaler")
    parser.add_argument("--input-dir", default="tests_input")
    parser.add_argument("--gpu-id", default="-1")
    parser.add_argument("--num-images", type=int, default=5, help="Number of images to test")
    parser.add_argument("--batch-size", type=int, default=2, help="Images per batch")
    args = parser.parse_args()

    print("=" * 80)
    print("REAL IMAGES TEST")
    print("=" * 80)
    print(f"Binary:      {args.binary}")
    print(f"Input dir:   {args.input_dir}")
    print(f"GPU ID:      {args.gpu_id}")
    print(f"Num images:  {args.num_images}")
    print(f"Batch size:  {args.batch_size}")
    print("=" * 80)

    # Load real images
    print("\nLoading images...")
    all_images = load_images(args.input_dir, limit=args.num_images)

    if not all_images:
        print("ERROR: No images found!")
        return 1

    total_size = sum(len(img[1]) for img in all_images)
    print(f"\nTotal: {len(all_images)} images, {total_size/1024/1024:.2f} MB")

    # Start process
    print("\nStarting keep-alive process...")
    cmd = [args.binary, "--mode", "stdin", "--keep-alive", "--gpu-id", args.gpu_id]
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    ps_proc = psutil.Process(proc.pid)

    time.sleep(1)
    mem_start = ps_proc.memory_info().rss / 1024 / 1024
    print(f"Process started, RAM: {mem_start:.1f} MB")

    # Process images in batches
    print(f"\nProcessing images (batch size {args.batch_size})...")
    print("-" * 80)

    results = []
    request_id = 1
    i = 0

    while i < len(all_images):
        # Create batch
        batch_end = min(i + args.batch_size, len(all_images))
        batch = [img[1] for img in all_images[i:batch_end]]
        batch_names = [img[0] for img in all_images[i:batch_end]]

        print(f"\nRequest {request_id}: Processing {len(batch)} images")
        for name in batch_names:
            print(f"  - {name}")

        # Send request
        frame = build_request(request_id, 0, "E", int(args.gpu_id), batch)

        start = time.time()
        proc.stdin.write(frame)
        proc.stdin.flush()

        # Read response
        resp_len_bytes = proc.stdout.read(4)
        if len(resp_len_bytes) < 4:
            print("  ERROR: No response")
            break

        resp_len = struct.unpack("<I", resp_len_bytes)[0]
        payload = proc.stdout.read(resp_len)
        elapsed = time.time() - start

        req_out, status, error, outputs = parse_response(payload)

        # Check memory
        mem_current = ps_proc.memory_info().rss / 1024 / 1024

        if status == 0 and len(outputs) == len(batch):
            total_out = sum(len(o) for o in outputs)
            print(f"  ✅ SUCCESS: {len(outputs)} outputs, {total_out/1024:.1f} KB")
            print(f"  Time: {elapsed:.2f}s, RAM: {mem_current:.1f} MB")
            results.append({
                'request_id': request_id,
                'batch_size': len(batch),
                'elapsed': elapsed,
                'ram': mem_current,
                'success': True
            })
        else:
            print(f"  ❌ FAILED: status={status}, error={error}")
            results.append({
                'request_id': request_id,
                'batch_size': len(batch),
                'elapsed': elapsed,
                'ram': mem_current,
                'success': False,
                'error': error
            })

        i = batch_end
        request_id += 1

    # Get final memory before shutdown
    try:
        mem_end = ps_proc.memory_info().rss / 1024 / 1024
    except:
        mem_end = mem_start

    # Shutdown
    proc.stdin.write(struct.pack("<I", 0))
    proc.stdin.close()
    proc.wait(timeout=10)

    # Summary
    print("\n" + "=" * 80)
    print("SUMMARY")
    print("=" * 80)

    successful = sum(1 for r in results if r['success'])
    total_images_processed = sum(r['batch_size'] for r in results if r['success'])
    total_time = sum(r['elapsed'] for r in results if r['success'])

    print(f"Requests:          {len(results)}")
    print(f"Successful:        {successful}/{len(results)}")
    print(f"Images processed:  {total_images_processed}")
    print(f"Total time:        {total_time:.2f}s")
    if total_time > 0:
        print(f"Throughput:        {total_images_processed/total_time:.2f} imgs/sec")
    print(f"RAM Start:         {mem_start:.1f} MB")
    print(f"RAM End:           {mem_end:.1f} MB")
    print(f"RAM Growth:        {mem_end - mem_start:+.1f} MB")

    print("\n" + "=" * 80)

    return 0 if successful == len(results) else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nInterrupted by user")
        sys.exit(1)
    except Exception as e:
        print(f"ERROR: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
