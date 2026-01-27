#!/usr/bin/env python3
import argparse
import base64
import subprocess
import struct
import sys

PNG_PIX = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR4nGNgYAAAAAMAASsJTYQAAAAASUVORK5CYII="
)

K_MAGIC = 0x42524452
K_VERSION = 2
MSG_TYPE_REQUEST = 1


def build_request(request_id, engine, meta, gpu_id, batch_count, images):
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
    frame = struct.pack("<I", len(header) + len(payload)) + header + payload
    return frame


def build_invalid_header(request_id):
    header = struct.pack("<IBBI", 0x12345678, K_VERSION, MSG_TYPE_REQUEST, request_id)
    payload = b""
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
    parser = argparse.ArgumentParser(description="Protocol v2 keep-alive stress test")
    parser.add_argument("--binary", default="bdreader-ncnn-upscaler", help="Path to binary")
    parser.add_argument("--gpu-id", default="-1", help="GPU identifier")
    args = parser.parse_args()

    cmd = [
        args.binary,
        "--mode",
        "stdin",
        "--keep-alive",
        "--gpu-id",
        args.gpu_id,
        "--log-protocol",
    ]

    proc = subprocess.Popen(
        cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    try:
        images = [PNG_PIX]
        for idx in range(1, 11):
            frame = build_request(idx, 0, "E", int(args.gpu_id), len(images), images)
            proc.stdin.write(frame)
            proc.stdin.flush()

            length_bytes = proc.stdout.read(4)
            if len(length_bytes) < 4:
                raise RuntimeError("missing response length")
            payload_len = struct.unpack("<I", length_bytes)[0]
            payload = proc.stdout.read(payload_len)
            request_id, status, error, outputs = parse_response(payload)
            print(f"ok request_id={request_id} status={status} error={error or 'none'} outputs={len(outputs)}")
            if status != 0 or len(outputs) != len(images):
                raise RuntimeError("unexpected status/outputs on valid request")

        # send invalid frame
        proc.stdin.write(build_invalid_header(999))
        proc.stdin.flush()
        length_bytes = proc.stdout.read(4)
        if len(length_bytes) < 4:
            raise RuntimeError("missing response for invalid frame")
        payload_len = struct.unpack("<I", length_bytes)[0]
        payload = proc.stdout.read(payload_len)
        request_id, status, error, outputs = parse_response(payload)
        print(f"invalid request_id={request_id} status={status} error={error}")
        if status == 0:
            raise RuntimeError("invalid frame should not return status 0")

        # ensure next valid request still works
        proc.stdin.write(build_request(1000, 0, "E", int(args.gpu_id), len(images), images))
        proc.stdin.flush()
        length_bytes = proc.stdout.read(4)
        payload_len = struct.unpack("<I", length_bytes)[0]
        payload = proc.stdout.read(payload_len)
        request_id, status, error, outputs = parse_response(payload)
        print(f"post-error request_id={request_id} status={status} outputs={len(outputs)}")
        if status != 0:
            raise RuntimeError("post-error request failed")

    finally:
        proc.stdin.write(struct.pack("<I", 0))
        proc.stdin.flush()
        proc.stdout.close()
        proc.stdin.close()
        proc.stderr.close()
        proc.wait()
    print("keep-alive stress test complete")


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        sys.stderr.write(f"{exc}\n")
        sys.exit(1)
