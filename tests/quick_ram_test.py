#!/usr/bin/env python3
"""Quick RAM & Performance tests with minimal 1x1 PNG"""
import base64
import psutil
import struct
import subprocess
import sys
import time

# Tiny 1x1 PNG (70 bytes)
PNG_1x1 = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR4nGNgYAAAAAMAASsJTYQAAAAASUVORK5CYII="
)

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


def send_and_wait(proc, req_id, images):
    frame = build_request(req_id, 0, "E", -1, images)
    start = time.time()
    proc.stdin.write(frame)
    proc.stdin.flush()

    resp_len_bytes = proc.stdout.read(4)
    if len(resp_len_bytes) < 4:
        return False, 0, 0

    resp_len = struct.unpack("<I", resp_len_bytes)[0]
    payload = proc.stdout.read(resp_len)
    latency = time.time() - start

    req_out, status, error, outputs = parse_response(payload)
    return status == 0 and req_out == req_id, latency, len(outputs)


def main():
    binary = "bdreader-ncnn-upscaler/build-release/bdreader-ncnn-upscaler"

    print("=" * 80)
    print("QUICK RAM & PERFORMANCE TESTS (1x1 PNG, CPU mode)")
    print("=" * 80)

    # Test 1: Memory leak detection (100 requests)
    print("\n=== Test 1: Memory Leak Detection (100 requests) ===")
    cmd = [binary, "--mode", "stdin", "--keep-alive", "--gpu-id", "-1"]
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    ps_proc = psutil.Process(proc.pid)

    time.sleep(1)
    mem_start = ps_proc.memory_info().rss / 1024 / 1024

    latencies = []
    mem_samples = [mem_start]

    try:
        num_reqs = 100
        for i in range(num_reqs):
            ok, lat, outputs = send_and_wait(proc, i + 1, [PNG_1x1] * 4)  # 4 images per batch

            if not ok:
                print(f"  ERROR at request {i+1}")
                break

            latencies.append(lat)
            mem_current = ps_proc.memory_info().rss / 1024 / 1024
            mem_samples.append(mem_current)

            if (i + 1) % 20 == 0:
                print(f"  Progress: {i+1}/{num_reqs}, RSS: {mem_current:.1f} MB, avg latency: {sum(latencies)/len(latencies):.3f}s")

        mem_end = ps_proc.memory_info().rss / 1024 / 1024
        mem_growth = mem_end - mem_start
        avg_lat = sum(latencies) / len(latencies)

        print(f"\n  Results:")
        print(f"    Requests:     {len(latencies)}")
        print(f"    RAM Start:    {mem_start:.1f} MB")
        print(f"    RAM End:      {mem_end:.1f} MB")
        print(f"    RAM Growth:   {mem_growth:+.1f} MB")
        print(f"    Avg Latency:  {avg_lat*1000:.1f} ms")

        if mem_growth > 50:
            print(f"    ⚠️  WARNING: Significant memory growth!")
        else:
            print(f"    ✅ Memory stable")

    finally:
        proc.stdin.write(struct.pack("<I", 0))
        proc.stdin.close()
        proc.wait(timeout=5)

    # Test 2: Batch size scaling
    print("\n=== Test 2: Batch Size Scaling (1, 2, 4, 8 images) ===")
    for batch_size in [1, 2, 4, 8]:
        cmd = [binary, "--mode", "stdin", "--keep-alive", "--gpu-id", "-1"]
        proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        time.sleep(0.5)

        images = [PNG_1x1] * batch_size
        times = []

        try:
            for i in range(10):
                ok, lat, outputs = send_and_wait(proc, i + 1, images)
                if ok:
                    times.append(lat)

            avg = sum(times) / len(times)
            print(f"  Batch {batch_size}: {avg*1000:.1f} ms avg ({len(times)} requests)")

        finally:
            proc.stdin.write(struct.pack("<I", 0))
            proc.stdin.close()
            proc.wait(timeout=5)

    # Test 3: Spawn vs Keep-Alive
    print("\n=== Test 3: Spawn vs Keep-Alive Performance ===")

    # Spawn mode (would need different invocation - skip for now)
    print("  (Spawn mode benchmark requires different test setup - see full test suite)")

    # Keep-alive throughput
    cmd = [binary, "--mode", "stdin", "--keep-alive", "--gpu-id", "-1"]
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(0.5)

    try:
        start_total = time.time()
        total_images = 0

        for i in range(50):
            ok, lat, outputs = send_and_wait(proc, i + 1, [PNG_1x1] * 2)
            if ok:
                total_images += outputs

        duration = time.time() - start_total
        throughput = total_images / duration

        print(f"  Keep-alive: {total_images} images in {duration:.2f}s = {throughput:.2f} imgs/sec")

    finally:
        proc.stdin.write(struct.pack("<I", 0))
        proc.stdin.close()
        proc.wait(timeout=5)

    print("\n" + "=" * 80)
    print("QUICK TESTS COMPLETED")
    print("=" * 80)


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print(f"ERROR: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
