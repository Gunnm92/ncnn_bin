#!/usr/bin/env python3
import argparse
import base64
import subprocess
import struct
import sys

# Tiny 1x1 PNG data (base64)
PNG_PIX = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR4nGNgYAAAAAMAASsJTYQAAAAASUVORK5CYII="
)

K_MAGIC = 0x42524452
K_VERSION = 2
MSG_TYPE_REQUEST = 1
MSG_TYPE_RESPONSE = 2


def build_request(request_id, engine, meta, gpu_id, batch_count, images):
    header = struct.pack("<IIII", K_MAGIC, K_VERSION, MSG_TYPE_REQUEST, request_id)
    payload = bytearray()
    payload.append(engine)
    payload.extend(struct.pack("<I", len(meta)))
    payload.extend(meta.encode())
    payload.extend(struct.pack("<i", gpu_id))
    payload.extend(struct.pack("<I", batch_count))
    for image in images:
        payload.extend(struct.pack("<I", len(image)))
        payload.extend(image)
    frame = struct.pack("<I", len(header) + len(payload)) + header + payload
    return frame


def build_frame_with_msg_type(request_id, msg_type):
    header = struct.pack("<IIII", K_MAGIC, K_VERSION, msg_type, request_id)
    payload = bytearray()
    payload.extend(struct.pack("<I", 0))
    return struct.pack("<I", len(header)) + header + payload


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


def main():
    parser = argparse.ArgumentParser(description="Protocol v2 integration smoke test")
    parser.add_argument("--binary", default="bdreader-ncnn-upscaler", help="Path to binary")
    parser.add_argument("--gpu-id", default="-1", help="GPU identifier (auto/-1)")
    args = parser.parse_args()

    images = [PNG_PIX, PNG_PIX]
    request_id = 42
    frame = build_request(request_id, 0, "E", int(args.gpu_id), len(images), images)

    cmd = [
        args.binary,
        "--mode",
        "stdin",
        "--keep-alive",
        "--gpu-id",
        args.gpu_id,
    ]

    proc = subprocess.Popen(
        cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    try:
        proc.stdin.write(frame)
        proc.stdin.flush()

        resp_len_bytes = proc.stdout.read(4)
        if len(resp_len_bytes) < 4:
            raise RuntimeError("no response length")
        resp_len = struct.unpack("<I", resp_len_bytes)[0]
        payload = proc.stdout.read(resp_len)
        request_id_out, status, error, outputs = parse_response(payload)

        print(f"request_id={request_id_out} status={status} error='{error}'")
        print(f"result_count={len(outputs)} expected={len(images)}")
        for idx, out in enumerate(outputs):
            print(f"  output[{idx}]= {len(out)} bytes")

        if status != 0:
            raise RuntimeError(f"non-zero status: {status}")
        if len(outputs) != len(images):
            raise RuntimeError("result_count mismatch")
        if request_id_out != request_id:
            raise RuntimeError("response request_id mismatch")
        print("  ✅ request_id echoed correctly")

        # Ensure we reject non-request msg_type headers (response frames)
        response_frame = build_frame_with_msg_type(77, MSG_TYPE_RESPONSE)
        proc.stdin.write(response_frame)
        proc.stdin.flush()
        resp_len_bytes = proc.stdout.read(4)
        if len(resp_len_bytes) < 4:
            raise RuntimeError("no response for msg_type test")
        resp_len = struct.unpack("<I", resp_len_bytes)[0]
        payload = proc.stdout.read(resp_len)
        request_id_rt, status_rt, error_rt, outputs_rt = parse_response(payload)
        print(f"msg_type test request_id={request_id_rt} status={status_rt} error={error_rt}")
        if request_id_rt != 77:
            raise RuntimeError("msg_type rejection returned wrong request_id")
        if status_rt == 0:
            raise RuntimeError("msg_type rejection should not succeed")
    finally:
        proc.stdin.write(struct.pack("<I", 0))
        proc.stdin.flush()
        proc.stdout.close()
        proc.stdin.close()
        proc.stderr.close()
        proc.wait()

    print("Protocol v2 integration script completed successfully")


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        sys.stderr.write(f"{exc}\n")
        sys.exit(1)
