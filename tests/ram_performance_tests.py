#!/usr/bin/env python3
"""
RAM & Performance Test Suite for NCNN Protocol v2
Tests memory usage, performance, and robustness under various conditions.
"""
import argparse
import os
import pathlib
import psutil
import struct
import subprocess
import sys
import time
from dataclasses import dataclass
from typing import List, Tuple

# Protocol constants
K_MAGIC = 0x42524452
K_VERSION = 2
MSG_TYPE_REQUEST = 1
STATUS_OK = 0


@dataclass
class MemorySnapshot:
    """Memory usage snapshot"""
    rss_mb: float  # Resident Set Size (actual RAM used)
    vms_mb: float  # Virtual Memory Size
    timestamp: float


@dataclass
class TestResult:
    """Test execution result"""
    name: str
    success: bool
    duration_sec: float
    requests: int
    images_processed: int
    mem_start: MemorySnapshot
    mem_peak: MemorySnapshot
    mem_end: MemorySnapshot
    mem_growth_mb: float
    avg_latency_ms: float
    throughput_imgs_sec: float
    error: str = ""


def build_request(request_id: int, engine: int, meta: str, gpu_id: int, images: List[bytes]) -> bytes:
    """Build a protocol v2 request frame"""
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
    frame = struct.pack("<I", len(header) + len(payload)) + header + payload
    return frame


def parse_response(data: bytes) -> Tuple[int, int, str, List[bytes]]:
    """Parse protocol v2 response"""
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


def get_memory_snapshot(proc: psutil.Process) -> MemorySnapshot:
    """Get current memory usage snapshot"""
    mem = proc.memory_info()
    return MemorySnapshot(
        rss_mb=mem.rss / 1024 / 1024,
        vms_mb=mem.vms / 1024 / 1024,
        timestamp=time.time()
    )


def load_test_images(input_dir: str, limit: int = None) -> List[Tuple[str, bytes]]:
    """Load test images from directory"""
    images = []
    input_path = pathlib.Path(input_dir)

    for ext in ['*.jpg', '*.jpeg', '*.png', '*.webp']:
        for img_path in sorted(input_path.glob(ext)):
            if limit and len(images) >= limit:
                break
            with open(img_path, 'rb') as f:
                images.append((img_path.name, f.read()))

    return images


def send_request_and_wait(proc: subprocess.Popen, request_id: int, images: List[bytes],
                          engine: int = 0, meta: str = "E", gpu_id: int = -1) -> Tuple[bool, float, int]:
    """Send a request and wait for response. Returns (success, latency_sec, output_count)"""
    frame = build_request(request_id, engine, meta, gpu_id, images)

    start = time.time()
    proc.stdin.write(frame)
    proc.stdin.flush()

    # Read response
    resp_len_bytes = proc.stdout.read(4)
    if len(resp_len_bytes) < 4:
        return False, time.time() - start, 0

    resp_len = struct.unpack("<I", resp_len_bytes)[0]
    payload = proc.stdout.read(resp_len)
    latency = time.time() - start

    req_id_out, status, error, outputs = parse_response(payload)

    if status != STATUS_OK or req_id_out != request_id or len(outputs) != len(images):
        return False, latency, len(outputs)

    return True, latency, len(outputs)


def test_batch_large_small_images(binary: str, input_dir: str, gpu_id: str) -> TestResult:
    """Test 1: Large batch with small images (no RAM explosion)"""
    print("\n=== Test 1: Large Batch with Small Images ===")

    # Load first 8 smallest images
    all_images = load_test_images(input_dir)
    all_images.sort(key=lambda x: len(x[1]))
    test_images = [img[1] for img in all_images[:8]]

    total_size = sum(len(img) for img in test_images)
    print(f"Using {len(test_images)} images, total size: {total_size/1024:.1f} KB")

    cmd = [binary, "--mode", "stdin", "--keep-alive", "--gpu-id", gpu_id]
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    ps_proc = psutil.Process(proc.pid)

    time.sleep(0.5)  # Let process initialize
    mem_start = get_memory_snapshot(ps_proc)
    mem_peak = mem_start

    start_time = time.time()
    success = True
    latencies = []

    try:
        # Send 5 requests with full batch
        for i in range(5):
            ok, latency, outputs = send_request_and_wait(proc, i + 1, test_images)
            latencies.append(latency)

            mem_current = get_memory_snapshot(ps_proc)
            if mem_current.rss_mb > mem_peak.rss_mb:
                mem_peak = mem_current

            if not ok:
                success = False
                print(f"  Request {i+1} failed")
                break
            print(f"  Request {i+1}: {outputs} outputs, {latency:.2f}s, RSS: {mem_current.rss_mb:.1f} MB")

        mem_end = get_memory_snapshot(ps_proc)
        duration = time.time() - start_time

    finally:
        proc.stdin.write(struct.pack("<I", 0))  # Shutdown
        proc.stdin.close()
        proc.wait(timeout=5)

    total_images = 5 * len(test_images) if success else 0
    avg_latency = sum(latencies) / len(latencies) if latencies else 0

    return TestResult(
        name="Large Batch (Small Images)",
        success=success,
        duration_sec=duration,
        requests=5,
        images_processed=total_images,
        mem_start=mem_start,
        mem_peak=mem_peak,
        mem_end=mem_end,
        mem_growth_mb=mem_end.rss_mb - mem_start.rss_mb,
        avg_latency_ms=avg_latency * 1000,
        throughput_imgs_sec=total_images / duration if duration > 0 else 0
    )


def test_heavy_images(binary: str, input_dir: str, gpu_id: str) -> TestResult:
    """Test 2: Heavy images (proper error handling, no OOM crash)"""
    print("\n=== Test 2: Heavy Images (Resource Limits) ===")

    # Load largest images
    all_images = load_test_images(input_dir)
    all_images.sort(key=lambda x: len(x[1]), reverse=True)
    test_images = [img[1] for img in all_images[:4]]  # Top 4 largest

    total_size = sum(len(img) for img in test_images)
    print(f"Using {len(test_images)} large images, total size: {total_size/1024/1024:.1f} MB")

    cmd = [binary, "--mode", "stdin", "--keep-alive", "--gpu-id", gpu_id]
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    ps_proc = psutil.Process(proc.pid)

    time.sleep(0.5)
    mem_start = get_memory_snapshot(ps_proc)
    mem_peak = mem_start

    start_time = time.time()
    success = True
    latencies = []

    try:
        # Send 3 requests with heavy images
        for i in range(3):
            ok, latency, outputs = send_request_and_wait(proc, i + 1, test_images)
            latencies.append(latency)

            mem_current = get_memory_snapshot(ps_proc)
            if mem_current.rss_mb > mem_peak.rss_mb:
                mem_peak = mem_current

            if not ok:
                print(f"  Request {i+1} failed (expected for resource limits)")
            else:
                print(f"  Request {i+1}: {outputs} outputs, {latency:.2f}s, RSS: {mem_current.rss_mb:.1f} MB")

        # Check process still alive after heavy load
        if proc.poll() is not None:
            success = False
            print("  ERROR: Process crashed!")
        else:
            print("  ✓ Process survived heavy images")

        mem_end = get_memory_snapshot(ps_proc)
        duration = time.time() - start_time

    finally:
        if proc.poll() is None:
            proc.stdin.write(struct.pack("<I", 0))
            proc.stdin.close()
            proc.wait(timeout=5)

    total_images = 3 * len(test_images)
    avg_latency = sum(latencies) / len(latencies) if latencies else 0

    return TestResult(
        name="Heavy Images",
        success=success,
        duration_sec=duration,
        requests=3,
        images_processed=total_images,
        mem_start=mem_start,
        mem_peak=mem_peak,
        mem_end=mem_end,
        mem_growth_mb=mem_end.rss_mb - mem_start.rss_mb,
        avg_latency_ms=avg_latency * 1000,
        throughput_imgs_sec=total_images / duration if duration > 0 else 0
    )


def test_long_session_memory_leak(binary: str, input_dir: str, gpu_id: str) -> TestResult:
    """Test 3: Long keep-alive session (50-100 requests) to detect memory leaks"""
    print("\n=== Test 3: Long Session Memory Leak Detection ===")

    # Use variety of images
    all_images = load_test_images(input_dir, limit=12)
    test_images = [img[1] for img in all_images]

    print(f"Using {len(test_images)} images for 50 requests")

    cmd = [binary, "--mode", "stdin", "--keep-alive", "--gpu-id", gpu_id]
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    ps_proc = psutil.Process(proc.pid)

    time.sleep(0.5)
    mem_start = get_memory_snapshot(ps_proc)
    mem_peak = mem_start

    # Track memory over time
    mem_samples = [mem_start]

    start_time = time.time()
    success = True
    total_processed = 0
    latencies = []

    try:
        num_requests = 50
        for i in range(num_requests):
            # Vary batch size (1-4 images)
            batch_size = (i % 4) + 1
            batch = test_images[:batch_size]

            ok, latency, outputs = send_request_and_wait(proc, i + 1, batch)
            latencies.append(latency)

            if ok:
                total_processed += outputs
            else:
                success = False
                print(f"  Request {i+1} failed")
                break

            mem_current = get_memory_snapshot(ps_proc)
            mem_samples.append(mem_current)

            if mem_current.rss_mb > mem_peak.rss_mb:
                mem_peak = mem_current

            if (i + 1) % 10 == 0:
                print(f"  Progress: {i+1}/{num_requests} requests, RSS: {mem_current.rss_mb:.1f} MB")

        mem_end = get_memory_snapshot(ps_proc)
        duration = time.time() - start_time

        # Analyze memory trend
        mem_growth = mem_end.rss_mb - mem_start.rss_mb
        leak_threshold_mb = 100  # More than 100 MB growth indicates potential leak

        if mem_growth > leak_threshold_mb:
            print(f"  WARNING: Significant memory growth: {mem_growth:.1f} MB")
        else:
            print(f"  ✓ Memory stable: growth {mem_growth:.1f} MB")

    finally:
        proc.stdin.write(struct.pack("<I", 0))
        proc.stdin.close()
        proc.wait(timeout=5)

    avg_latency = sum(latencies) / len(latencies) if latencies else 0

    return TestResult(
        name="Long Session (50 requests)",
        success=success,
        duration_sec=duration,
        requests=50,
        images_processed=total_processed,
        mem_start=mem_start,
        mem_peak=mem_peak,
        mem_end=mem_end,
        mem_growth_mb=mem_end.rss_mb - mem_start.rss_mb,
        avg_latency_ms=avg_latency * 1000,
        throughput_imgs_sec=total_processed / duration if duration > 0 else 0
    )


def test_spawn_vs_keepalive_benchmark(binary: str, input_dir: str, gpu_id: str) -> TestResult:
    """Test 4: Performance comparison - spawn per request vs keep-alive"""
    print("\n=== Test 4: Spawn vs Keep-Alive Benchmark ===")

    # Use 3 small images for quick tests
    all_images = load_test_images(input_dir, limit=3)
    test_images = [img[1] for img in all_images]

    num_iterations = 10

    # Benchmark 1: Spawn per request (legacy mode)
    print(f"\nBenchmark 1: Spawn mode ({num_iterations} spawns)")
    spawn_times = []

    for i in range(num_iterations):
        cmd = [binary, "--mode", "stdin", "--gpu-id", gpu_id]
        start = time.time()

        proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

        # Send all images at once (legacy mode reads until EOF)
        for img in test_images:
            proc.stdin.write(struct.pack("<I", len(img)))
            proc.stdin.write(img)
        proc.stdin.close()

        # Wait for process to finish
        proc.wait(timeout=30)
        elapsed = time.time() - start
        spawn_times.append(elapsed)

        if (i + 1) % 5 == 0:
            print(f"  Progress: {i+1}/{num_iterations}, avg: {sum(spawn_times)/len(spawn_times):.2f}s")

    spawn_avg = sum(spawn_times) / len(spawn_times)
    spawn_total = sum(spawn_times)

    # Benchmark 2: Keep-alive mode
    print(f"\nBenchmark 2: Keep-alive mode ({num_iterations} requests)")
    cmd = [binary, "--mode", "stdin", "--keep-alive", "--gpu-id", gpu_id]
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    ps_proc = psutil.Process(proc.pid)

    time.sleep(0.5)
    mem_start = get_memory_snapshot(ps_proc)

    keepalive_times = []
    start_total = time.time()

    try:
        for i in range(num_iterations):
            ok, latency, outputs = send_request_and_wait(proc, i + 1, test_images)
            keepalive_times.append(latency)

            if (i + 1) % 5 == 0:
                print(f"  Progress: {i+1}/{num_iterations}, avg: {sum(keepalive_times)/len(keepalive_times):.2f}s")

        keepalive_total = time.time() - start_total
        keepalive_avg = sum(keepalive_times) / len(keepalive_times)

        mem_end = get_memory_snapshot(ps_proc)

    finally:
        proc.stdin.write(struct.pack("<I", 0))
        proc.stdin.close()
        proc.wait(timeout=5)

    # Results
    speedup = spawn_total / keepalive_total
    print(f"\n  Results:")
    print(f"    Spawn mode:      {spawn_total:.2f}s total, {spawn_avg:.2f}s avg")
    print(f"    Keep-alive mode: {keepalive_total:.2f}s total, {keepalive_avg:.2f}s avg")
    print(f"    Speedup:         {speedup:.2f}x")

    return TestResult(
        name=f"Spawn vs Keep-Alive ({num_iterations} iterations)",
        success=True,
        duration_sec=keepalive_total,
        requests=num_iterations,
        images_processed=num_iterations * len(test_images),
        mem_start=mem_start,
        mem_peak=mem_end,
        mem_end=mem_end,
        mem_growth_mb=mem_end.rss_mb - mem_start.rss_mb,
        avg_latency_ms=keepalive_avg * 1000,
        throughput_imgs_sec=(num_iterations * len(test_images)) / keepalive_total,
        error=f"Speedup: {speedup:.2f}x vs spawn"
    )


def test_backpressure_stress(binary: str, input_dir: str, gpu_id: str) -> TestResult:
    """Test 5: Backpressure - send requests faster than GPU can process"""
    print("\n=== Test 5: Backpressure / Stress Test ===")

    # Use 8 medium images
    all_images = load_test_images(input_dir, limit=8)
    test_images = [img[1] for img in all_images]

    print(f"Sending 20 rapid requests with {len(test_images)} images each")

    cmd = [binary, "--mode", "stdin", "--keep-alive", "--gpu-id", gpu_id]
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    ps_proc = psutil.Process(proc.pid)

    time.sleep(0.5)
    mem_start = get_memory_snapshot(ps_proc)
    mem_peak = mem_start

    start_time = time.time()
    success = True
    total_processed = 0
    latencies = []

    try:
        num_requests = 20

        for i in range(num_requests):
            ok, latency, outputs = send_request_and_wait(proc, i + 1, test_images)
            latencies.append(latency)

            mem_current = get_memory_snapshot(ps_proc)
            if mem_current.rss_mb > mem_peak.rss_mb:
                mem_peak = mem_current

            if ok:
                total_processed += outputs
            else:
                success = False
                print(f"  Request {i+1} failed under stress")
                break

            if (i + 1) % 5 == 0:
                print(f"  Progress: {i+1}/{num_requests}, RSS: {mem_current.rss_mb:.1f} MB, latency: {latency:.2f}s")

        mem_end = get_memory_snapshot(ps_proc)
        duration = time.time() - start_time

        print(f"  ✓ Completed {num_requests} rapid requests without failure")

    finally:
        proc.stdin.write(struct.pack("<I", 0))
        proc.stdin.close()
        proc.wait(timeout=5)

    avg_latency = sum(latencies) / len(latencies) if latencies else 0

    return TestResult(
        name="Backpressure Stress",
        success=success,
        duration_sec=duration,
        requests=20,
        images_processed=total_processed,
        mem_start=mem_start,
        mem_peak=mem_peak,
        mem_end=mem_end,
        mem_growth_mb=mem_end.rss_mb - mem_start.rss_mb,
        avg_latency_ms=avg_latency * 1000,
        throughput_imgs_sec=total_processed / duration if duration > 0 else 0
    )


def print_summary(results: List[TestResult]):
    """Print test summary report"""
    print("\n" + "=" * 80)
    print("TEST SUMMARY")
    print("=" * 80)

    for result in results:
        status = "✓ PASS" if result.success else "✗ FAIL"
        print(f"\n{status} {result.name}")
        print(f"  Duration:       {result.duration_sec:.2f}s")
        print(f"  Requests:       {result.requests}")
        print(f"  Images:         {result.images_processed}")
        print(f"  Throughput:     {result.throughput_imgs_sec:.2f} imgs/sec")
        print(f"  Avg Latency:    {result.avg_latency_ms:.2f} ms")
        print(f"  RAM Start:      {result.mem_start.rss_mb:.1f} MB")
        print(f"  RAM Peak:       {result.mem_peak.rss_mb:.1f} MB")
        print(f"  RAM End:        {result.mem_end.rss_mb:.1f} MB")
        print(f"  RAM Growth:     {result.mem_growth_mb:+.1f} MB")
        if result.error:
            print(f"  Note:           {result.error}")

    print("\n" + "=" * 80)

    passed = sum(1 for r in results if r.success)
    total = len(results)
    print(f"FINAL RESULT: {passed}/{total} tests passed")
    print("=" * 80)


def main():
    parser = argparse.ArgumentParser(description="RAM & Performance Test Suite for NCNN Protocol v2")
    parser.add_argument("--binary", default="bdreader-ncnn-upscaler/build-release/bdreader-ncnn-upscaler",
                        help="Path to bdreader-ncnn-upscaler binary")
    parser.add_argument("--input-dir", default="tests_input",
                        help="Directory containing test images")
    parser.add_argument("--gpu-id", default="-1",
                        help="GPU ID to use (-1 for auto)")
    parser.add_argument("--tests", default="all",
                        help="Tests to run: all, batch, heavy, leak, bench, stress (comma-separated)")
    args = parser.parse_args()

    # Check binary exists
    if not os.path.exists(args.binary):
        print(f"ERROR: Binary not found: {args.binary}")
        return 1

    # Check input directory
    if not os.path.exists(args.input_dir):
        print(f"ERROR: Input directory not found: {args.input_dir}")
        return 1

    print("=" * 80)
    print("RAM & PERFORMANCE TEST SUITE")
    print("=" * 80)
    print(f"Binary:     {args.binary}")
    print(f"Input dir:  {args.input_dir}")
    print(f"GPU ID:     {args.gpu_id}")
    print("=" * 80)

    results = []
    tests_to_run = args.tests.lower().split(',')

    try:
        if 'all' in tests_to_run or 'batch' in tests_to_run:
            results.append(test_batch_large_small_images(args.binary, args.input_dir, args.gpu_id))

        if 'all' in tests_to_run or 'heavy' in tests_to_run:
            results.append(test_heavy_images(args.binary, args.input_dir, args.gpu_id))

        if 'all' in tests_to_run or 'leak' in tests_to_run:
            results.append(test_long_session_memory_leak(args.binary, args.input_dir, args.gpu_id))

        if 'all' in tests_to_run or 'bench' in tests_to_run:
            results.append(test_spawn_vs_keepalive_benchmark(args.binary, args.input_dir, args.gpu_id))

        if 'all' in tests_to_run or 'stress' in tests_to_run:
            results.append(test_backpressure_stress(args.binary, args.input_dir, args.gpu_id))

    except Exception as e:
        print(f"\nFATAL ERROR: {e}")
        import traceback
        traceback.print_exc()
        return 1

    print_summary(results)

    # Exit code: 0 if all passed, 1 if any failed
    all_passed = all(r.success for r in results)
    return 0 if all_passed else 1


if __name__ == "__main__":
    sys.exit(main())
