#!/usr/bin/env python3
"""
Compare batch processing vs single image processing.
Tests throughput and latency differences.
"""
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


def send_request(proc, request_id, images):
    """Send request and return (success, elapsed_time, outputs)"""
    frame = build_request(request_id, 0, "E", int(args.gpu_id), images)

    start = time.time()
    proc.stdin.write(frame)
    proc.stdin.flush()

    resp_len_bytes = proc.stdout.read(4)
    if len(resp_len_bytes) < 4:
        return False, 0, []

    resp_len = struct.unpack("<I", resp_len_bytes)[0]
    payload = proc.stdout.read(resp_len)
    elapsed = time.time() - start

    req_out, status, error, outputs = parse_response(payload)
    return status == 0 and len(outputs) == len(images), elapsed, outputs


def load_images(input_dir, pattern="*.jpg", limit=20):
    """Load test images"""
    images = []
    for img_path in sorted(pathlib.Path(input_dir).glob(pattern))[:limit]:
        with open(img_path, 'rb') as f:
            images.append((img_path.name, f.read()))
    return images


def test_mode(binary, gpu_id, images, batch_size, mode_name):
    """Test with specific batch size"""
    print(f"\n{'='*80}")
    print(f"Mode: {mode_name}")
    print(f"Batch size: {batch_size}")
    print(f"{'='*80}")

    # Start process
    cmd = [binary, "--mode", "stdin", "--keep-alive", "--gpu-id", gpu_id]
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    ps_proc = psutil.Process(proc.pid)

    time.sleep(0.5)
    mem_start = ps_proc.memory_info().rss / 1024 / 1024

    request_times = []
    total_images_processed = 0
    request_id = 1
    i = 0

    start_total = time.time()

    while i < len(images):
        batch_end = min(i + batch_size, len(images))
        batch = [img[1] for img in images[i:batch_end]]

        success, elapsed, outputs = send_request(proc, request_id, batch)

        if success:
            request_times.append(elapsed)
            total_images_processed += len(outputs)

            if request_id <= 3 or (request_id % 5 == 0):  # Print first 3 + every 5th
                mem_current = ps_proc.memory_info().rss / 1024 / 1024
                print(f"  Request {request_id:3d}: {len(batch)} imgs, {elapsed:.3f}s ({elapsed/len(batch)*1000:.1f} ms/img), RAM: {mem_current:.0f} MB")
        else:
            print(f"  Request {request_id} FAILED")

        i = batch_end
        request_id += 1

    total_time = time.time() - start_total

    try:
        mem_end = ps_proc.memory_info().rss / 1024 / 1024
    except:
        mem_end = mem_start

    # Shutdown
    proc.stdin.write(struct.pack("<I", 0))
    proc.stdin.close()
    proc.wait(timeout=10)

    # Calculate metrics
    avg_request_time = sum(request_times) / len(request_times) if request_times else 0
    throughput = total_images_processed / total_time if total_time > 0 else 0
    avg_latency_per_img = (sum(request_times) / total_images_processed * 1000) if total_images_processed > 0 else 0

    return {
        'mode': mode_name,
        'batch_size': batch_size,
        'total_images': total_images_processed,
        'total_requests': len(request_times),
        'total_time': total_time,
        'avg_request_time': avg_request_time,
        'throughput_imgs_sec': throughput,
        'avg_latency_ms_per_img': avg_latency_per_img,
        'mem_start': mem_start,
        'mem_end': mem_end,
        'mem_growth': mem_end - mem_start
    }


def print_results(results):
    """Print comparison table"""
    print("\n" + "="*80)
    print("COMPARISON RESULTS")
    print("="*80)

    print("\n📊 Performance Metrics")
    print("-" * 80)
    print(f"{'Mode':<20} {'Batch':<8} {'Images':<8} {'Requests':<10} {'Total Time':<12} {'Throughput':<15}")
    print("-" * 80)

    for r in results:
        print(f"{r['mode']:<20} {r['batch_size']:<8} {r['total_images']:<8} "
              f"{r['total_requests']:<10} {r['total_time']:>8.2f}s    "
              f"{r['throughput_imgs_sec']:>8.3f} imgs/s")

    print("\n⚡ Latency Comparison")
    print("-" * 80)
    print(f"{'Mode':<20} {'Batch':<8} {'Avg Request Time':<18} {'Avg Latency/Img':<18}")
    print("-" * 80)

    for r in results:
        print(f"{r['mode']:<20} {r['batch_size']:<8} "
              f"{r['avg_request_time']:>12.3f}s      "
              f"{r['avg_latency_ms_per_img']:>12.1f} ms")

    print("\n💾 Memory Usage")
    print("-" * 80)
    print(f"{'Mode':<20} {'Batch':<8} {'Start':<12} {'End':<12} {'Growth':<12}")
    print("-" * 80)

    for r in results:
        print(f"{r['mode']:<20} {r['batch_size']:<8} "
              f"{r['mem_start']:>8.0f} MB  "
              f"{r['mem_end']:>8.0f} MB  "
              f"{r['mem_growth']:>+8.0f} MB")

    # Calculate speedups
    if len(results) > 1:
        baseline = results[0]  # Single image mode

        print("\n🚀 Speedup vs Single Image Mode (Baseline)")
        print("-" * 80)
        print(f"{'Mode':<20} {'Batch':<8} {'Throughput Gain':<20} {'Efficiency':<15}")
        print("-" * 80)

        for r in results:
            if r['batch_size'] == 1:
                print(f"{r['mode']:<20} {r['batch_size']:<8} {'1.00x (baseline)':<20} {'100%':<15}")
            else:
                speedup = r['throughput_imgs_sec'] / baseline['throughput_imgs_sec']
                efficiency = (speedup / r['batch_size']) * 100  # How much of linear scaling achieved
                print(f"{r['mode']:<20} {r['batch_size']:<8} "
                      f"{speedup:>6.2f}x faster      "
                      f"{efficiency:>6.1f}%")

        print("\n📝 Notes:")
        print("  • Efficiency = (Actual speedup / Batch size) × 100%")
        print("  • 100% = perfect linear scaling (2x batch = 2x throughput)")
        print("  • > 80% = excellent batching efficiency")

    print("\n" + "="*80)


def main():
    global args
    parser = argparse.ArgumentParser(description="Compare batch vs single image processing")
    parser.add_argument("--binary", default="bdreader-ncnn-upscaler/build-release/bdreader-ncnn-upscaler")
    parser.add_argument("--input-dir", default="tests_input")
    parser.add_argument("--gpu-id", default="0")
    parser.add_argument("--num-images", type=int, default=20, help="Total images to test")
    parser.add_argument("--batch-sizes", default="1,2,4,8", help="Comma-separated batch sizes to test")
    args = parser.parse_args()

    print("="*80)
    print("BATCH vs SINGLE IMAGE COMPARISON")
    print("="*80)
    print(f"Binary:       {args.binary}")
    print(f"Input dir:    {args.input_dir}")
    print(f"GPU ID:       {args.gpu_id}")
    print(f"Total images: {args.num_images}")
    print(f"Batch sizes:  {args.batch_sizes}")
    print("="*80)

    # Load images
    print("\nLoading images...")
    all_images = load_images(args.input_dir, limit=args.num_images)

    if not all_images:
        print("ERROR: No images found!")
        return 1

    print(f"Loaded {len(all_images)} images")
    total_size = sum(len(img[1]) for img in all_images)
    print(f"Total size: {total_size/1024/1024:.2f} MB")

    # Test each batch size
    batch_sizes = [int(x.strip()) for x in args.batch_sizes.split(',')]
    results = []

    for batch_size in batch_sizes:
        mode_name = f"Single Image" if batch_size == 1 else f"Batch {batch_size}"
        result = test_mode(args.binary, args.gpu_id, all_images, batch_size, mode_name)
        results.append(result)
        time.sleep(1)  # Cool down between tests

    # Print comparison
    print_results(results)

    return 0


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
