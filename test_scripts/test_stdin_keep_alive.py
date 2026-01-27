#!/usr/bin/env python3
"""
Test stdin --keep-alive framed protocol with NCNN binary.

stdin  : [size:u32_le][bytes...] repeated (size=0 ends)
stdout : [status:u32_le][size:u32_le][bytes...] repeated (status=0 ok)
"""

import struct
import subprocess
import sys
import os
from pathlib import Path


def u32(value: int) -> bytes:
    return struct.pack("<I", value)


def read_u32(buf: bytes, offset: int) -> int:
    return struct.unpack_from("<I", buf, offset)[0]


def test_keep_alive_framed() -> bool:
    img_dir = Path("img_test")
    # Pick 10 images deterministically from img_test (prefer jpg).
    candidates = sorted(img_dir.glob("*.jpg"))
    if len(candidates) < 10:
        candidates.extend(sorted(img_dir.glob("*.png")))
        candidates.extend(sorted(img_dir.glob("*.webp")))
    test_images = candidates[:10]
    if len(test_images) < 10:
        print(f"Error: Need 10 test images in {img_dir}, found {len(test_images)}")
        return False

    image_data = []
    for img_path in test_images:
        if not img_path.exists():
            print(f"Error: Test image not found: {img_path}")
            return False
        image_data.append(img_path.read_bytes())

    stdin_payload = bytearray()
    for data in image_data:
        stdin_payload.extend(u32(len(data)))
        stdin_payload.extend(data)
    stdin_payload.extend(u32(0))  # stop frame

    candidate_bins = [
        Path("build") / "bdreader-ncnn-upscaler",
        Path("bdreader-ncnn-upscaler") / "build-release" / "bdreader-ncnn-upscaler",
    ]
    bin_path = next(
        (p for p in candidate_bins if p.exists() and p.is_file() and p.stat().st_size > 0 and os.access(p, os.X_OK)),
        None,
    )
    if bin_path is None:
        print("Error: Could not find bdreader-ncnn-upscaler binary (build or build-release)")
        return False

    cmd = [
        str(bin_path),
        "--engine",
        "realcugan",
        "--mode",
        "stdin",
        "--keep-alive",
        "--quality",
        "F",
        "--model",
        "/config/workspace/BDReader-Rust/backend/models/realcugan/models-se",
        "--gpu-id",
        "0",
        "--format",
        "webp",
    ]

    try:
        proc = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        stdout, stderr = proc.communicate(input=bytes(stdin_payload), timeout=300)
    except subprocess.TimeoutExpired:
        proc.kill()
        print("Error: Process timed out")
        return False

    if proc.returncode != 0:
        print(f"Error: Process returned {proc.returncode}")
        print(stderr.decode("utf-8", errors="replace"))
        return False

    # Parse framed stdout: status + size + data, repeated
    offset = 0
    results = []
    for i in range(len(image_data)):
        if offset + 8 > len(stdout):
            print(f"Error: Output too short for frame {i}")
            return False
        status = read_u32(stdout, offset)
        size = read_u32(stdout, offset + 4)
        offset += 8

        if status != 0:
            print(f"Error: Frame {i} status={status}")
            return False
        if offset + size > len(stdout):
            print(f"Error: Frame {i} size mismatch")
            return False

        data = stdout[offset : offset + size]
        offset += size
        results.append(data)

    # Save results (sanity check that output isn't empty)
    out_dir = Path("results")
    out_dir.mkdir(exist_ok=True)
    for i, data in enumerate(results):
        if not data:
            print(f"Error: Empty output for frame {i}")
            return False
        (out_dir / f"stdin_keep_alive_test_{i}.webp").write_bytes(data)

    return True


if __name__ == "__main__":
    ok = test_keep_alive_framed()
    print("✅ stdin keep-alive framed test passed!" if ok else "❌ stdin keep-alive framed test failed!")
    sys.exit(0 if ok else 1)
