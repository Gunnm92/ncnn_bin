#!/usr/bin/env python3
"""
Benchmark NCNN pipeline with different queue capacities
"""
import sys
import time
import struct
import subprocess
from pathlib import Path

def benchmark_batch(num_images: int, queue_capacity: int, test_image: Path) -> dict:
    """Benchmark batch processing with specific queue capacity"""
    
    # Read test image
    with open(test_image, 'rb') as f:
        image_data = f.read()
    
    # Build payload
    payload = bytearray()
    payload.extend(struct.pack('<I', num_images))
    
    for _ in range(num_images):
        payload.extend(struct.pack('<I', len(image_data)))
        payload.extend(image_data)
    
    # Run binary with timing
    binary = Path("/config/workspace/BDReader-Rust/ncnn_bin/build/bdreader-ncnn-upscaler")
    
    cmd = [
        str(binary),
        '--engine', 'realcugan',
        '--mode', 'stdin',
        '--batch-size', str(num_images),
        '--quality', 'F',
        '--model', '/config/workspace/BDReader-Rust/backend/models/realcugan/models-se',
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
    
    num_results = struct.unpack('<I', stdout[:4])[0]
    
    return {
        'success': True,
        'num_images': num_images,
        'queue_capacity': queue_capacity,
        'elapsed_seconds': elapsed,
        'images_per_second': num_images / elapsed,
        'ms_per_image': (elapsed * 1000) / num_images,
        'num_results': num_results,
    }

def main():
    test_image = Path("/config/workspace/BDReader-Rust/backend/benches/images/0026_jpg.rf.f513d3f34c46bc9e458b6d82f1707f76.jpg")
    
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
