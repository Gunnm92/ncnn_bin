#!/usr/bin/env python3
"""
Benchmark NCNN pipeline with different queue capacities
"""
import sys
import time
import struct
import subprocess
from pathlib import Path

K_MAGIC = 0x42524452
K_VERSION = 2
MSG_TYPE_REQUEST = 1


def build_request_frame(request_id: int,
                        engine: int,
                        meta: str,
                        gpu_id: int,
                        batch_count: int,
                        images: list[bytes]) -> bytes:
    header = struct.pack("<IBBI", K_MAGIC, K_VERSION, MSG_TYPE_REQUEST, request_id)
    payload = bytearray()
    payload.append(engine)
    payload.extend(struct.pack("<I", len(meta)))
    payload.extend(meta.encode())
    payload.extend(struct.pack("<i", gpu_id))
    payload.extend(struct.pack("<I", batch_count))
    for image in images:
        payload.extend(struct.pack("<I", len(image)))
        payload.extend(image)
    total_len = len(header) + len(payload)
    return struct.pack("<I", total_len) + header + payload


def parse_response(payload: bytes) -> tuple[int, int, str]:
    offset = 0
    request_id, status, error_len = struct.unpack_from("<III", payload, offset)
    offset += 12
    error = payload[offset : offset + error_len].decode("utf-8", "replace")
    offset += error_len
    result_count = struct.unpack_from("<I", payload, offset)[0]
    return status, result_count, error

def benchmark_batch(num_images: int, queue_capacity: int, test_image: Path) -> dict:
    """Benchmark batch processing with specific queue capacity"""
    
    # Read test image
    with open(test_image, 'rb') as f:
        image_data = f.read()
    
    # Build framed request (protocol v2)
    images = [image_data] * num_images
    frame = build_request_frame(
        request_id=1,
        engine=0,
        meta='F',
        gpu_id=0,
        batch_count=num_images,
        images=images
    )
    shutdown_frame = struct.pack('<I', 0)
    payload = frame + shutdown_frame
    
    repo_root = Path(__file__).resolve().parent
    binary = repo_root / "bdreader-ncnn-upscaler" / "build-release" / "bdreader-ncnn-upscaler"
    model_path = repo_root / "models" / "realcugan" / "models-se"
    
    cmd = [
        str(binary),
        '--engine', 'realcugan',
        '--mode', 'stdin',
        '--keep-alive',
        '--quality', 'F',
        '--model', str(model_path),
        '--gpu-id', '0',
    ]
    
    start = time.time()
    result = subprocess.run(
        cmd,
        input=bytes(payload),
        capture_output=True,
        timeout=180
    )
    elapsed = time.time() - start
    
    if result.returncode != 0:
        return {'success': False, 'error': 'Binary failed'}
    
    # Parse results
    stdout = result.stdout
    if len(stdout) < 4:
        return {'success': False, 'error': 'Invalid output'}
    
    payload_len = struct.unpack('<I', stdout[:4])[0]
    if len(stdout) < 4 + payload_len:
        return {'success': False, 'error': 'Truncated output'}
    
    status, result_count, error_msg = parse_response(stdout[4:4 + payload_len])
    if status != 0:
        return {'success': False, 'error': f'Non-zero status {status}: {error_msg}'}
    
    return {
        'success': True,
        'num_images': num_images,
        'queue_capacity': queue_capacity,
        'elapsed_seconds': elapsed,
        'images_per_second': num_images / elapsed,
        'ms_per_image': (elapsed * 1000) / num_images,
        'num_results': result_count,
    }

def main():
    repo_root = Path(__file__).resolve().parent
    test_image = repo_root / "img_test" / "P00003.jpg"
    
    print("="*70)
    print("NCNN Pipeline Performance Benchmark")
    print("="*70)
    print(f"Test image: {test_image.name} ({test_image.stat().st_size / 1024:.1f} KB)")
    print()
    
    # Benchmark different batch sizes
    batch_sizes = [10, 20, 50]
    
    results = []
    for batch_size in batch_sizes:
        print(f"Testing batch_size={batch_size}...", flush=True)
        result = benchmark_batch(batch_size, 1, test_image)
        
        if result['success']:
            results.append(result)
            print(f"  ✅ {result['elapsed_seconds']:.2f}s total | "
                  f"{result['ms_per_image']:.1f}ms/image | "
                  f"{result['images_per_second']:.2f} img/s")
        else:
            print(f"  ❌ {result.get('error', 'Unknown error')}")
        print()
    
    # Summary
    print("="*70)
    print("Performance Summary")
    print("="*70)
    print(f"{'Batch Size':<12} {'Total Time':<12} {'ms/image':<12} {'Images/sec':<12}")
    print("-"*70)
    for r in results:
        print(f"{r['num_images']:<12} {r['elapsed_seconds']:<12.2f} "
              f"{r['ms_per_image']:<12.1f} {r['images_per_second']:<12.2f}")
    
    if results:
        avg_ms = sum(r['ms_per_image'] for r in results) / len(results)
        print("-"*70)
        print(f"Average: {avg_ms:.1f} ms/image")

if __name__ == '__main__':
    main()
