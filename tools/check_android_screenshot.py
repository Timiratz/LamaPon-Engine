"""Inspect an Android screencap PNG for the Native Smoke scene's green UI marker."""
from __future__ import annotations

import argparse
import binascii
import json
import struct
import sys
import zlib


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
MAX_PIXELS = 32 * 1024 * 1024


def _paeth(left: int, above: int, upper_left: int) -> int:
    estimate = left + above - upper_left
    left_distance = abs(estimate - left)
    above_distance = abs(estimate - above)
    upper_left_distance = abs(estimate - upper_left)
    if left_distance <= above_distance and left_distance <= upper_left_distance:
        return left
    if above_distance <= upper_left_distance:
        return above
    return upper_left


def decode_android_png(data: bytes) -> tuple[int, int, int, bytes]:
    """Decode non-interlaced 8-bit RGB/RGBA PNGs without writing temporary files."""
    if not isinstance(data, bytes) or not data.startswith(PNG_SIGNATURE):
        raise ValueError("Android screenshot is not a PNG")

    offset = len(PNG_SIGNATURE)
    width = height = bit_depth = color_type = interlace = None
    compressed = bytearray()
    saw_end = False
    saw_header = False
    while offset + 12 <= len(data):
        length = struct.unpack_from(">I", data, offset)[0]
        chunk_type = data[offset + 4:offset + 8]
        chunk_end = offset + 12 + length
        if chunk_end > len(data):
            raise ValueError("Android screenshot has a truncated PNG chunk")
        chunk = data[offset + 8:offset + 8 + length]
        expected_crc = struct.unpack_from(">I", data, offset + 8 + length)[0]
        if binascii.crc32(chunk_type + chunk) & 0xffffffff != expected_crc:
            raise ValueError("Android screenshot has a corrupt PNG chunk")
        if not saw_header:
            if chunk_type != b"IHDR" or length != 13:
                raise ValueError("Android screenshot is missing its PNG header")
            width, height, bit_depth, color_type, compression, filtering, interlace = struct.unpack(
                ">IIBBBBB", chunk)
            if not width or not height or width * height > MAX_PIXELS:
                raise ValueError("Android screenshot dimensions are invalid or too large")
            if bit_depth != 8 or color_type not in (2, 6) or compression or filtering or interlace:
                raise ValueError("Android screenshot must be a non-interlaced 8-bit RGB/RGBA PNG")
            saw_header = True
        elif chunk_type == b"IHDR":
            raise ValueError("Android screenshot contains multiple PNG headers")
        elif chunk_type == b"IDAT":
            compressed.extend(chunk)
        elif chunk_type == b"IEND":
            if length:
                raise ValueError("Android screenshot has an invalid PNG end chunk")
            saw_end = True
            offset = chunk_end
            break
        offset = chunk_end

    if not saw_end and offset < len(data):
        raise ValueError("Android screenshot has a truncated PNG chunk")
    if not saw_header or not saw_end or offset != len(data):
        raise ValueError("Android screenshot has an incomplete PNG stream")

    channels = 4 if color_type == 6 else 3
    stride = width * channels
    expected_size = height * (stride + 1)
    decompressor = zlib.decompressobj()
    try:
        raw = decompressor.decompress(compressed, expected_size + 1)
    except zlib.error as error:
        raise ValueError("Android screenshot has a corrupt PNG image stream") from error
    if len(raw) != expected_size or not decompressor.eof or decompressor.unused_data:
        raise ValueError("Android screenshot has an invalid PNG image stream")

    pixels = bytearray(height * stride)
    source_offset = 0
    for y in range(height):
        filter_type = raw[source_offset]
        source_offset += 1
        encoded = raw[source_offset:source_offset + stride]
        source_offset += stride
        if filter_type > 4:
            raise ValueError("Android screenshot uses an invalid PNG row filter")
        row_offset = y * stride
        previous_offset = row_offset - stride
        for x, byte in enumerate(encoded):
            left = pixels[row_offset + x - channels] if x >= channels else 0
            above = pixels[previous_offset + x] if y else 0
            upper_left = pixels[previous_offset + x - channels] if y and x >= channels else 0
            if filter_type == 1:
                predictor = left
            elif filter_type == 2:
                predictor = above
            elif filter_type == 3:
                predictor = (left + above) // 2
            elif filter_type == 4:
                predictor = _paeth(left, above, upper_left)
            else:
                predictor = 0
            pixels[row_offset + x] = (byte + predictor) & 0xff

    return width, height, channels, bytes(pixels)


def count_green_marker(data: bytes) -> dict:
    """Count bright-green pixels used by tests/native/assets/scenes/Main.scene.json."""
    width, height, channels, pixels = decode_android_png(data)
    count = 0
    for offset in range(0, len(pixels), channels):
        red, green, blue = pixels[offset:offset + 3]
        if red < 48 and green > 160 and blue < 48:
            count += 1
    return {"width": width, "height": height, "greenMarkerPixels": count}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--min-green-pixels", type=int, default=1000)
    arguments = parser.parse_args()
    if arguments.min_green_pixels < 1:
        parser.error("--min-green-pixels must be positive")
    try:
        result = count_green_marker(sys.stdin.buffer.read())
        print(json.dumps(result))
        if result["greenMarkerPixels"] < arguments.min_green_pixels:
            print("Android screenshot does not show enough of the expected green UI marker", file=sys.stderr)
            return 1
        return 0
    except (ValueError, zlib.error) as error:
        print("Cannot verify Android screenshot: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
