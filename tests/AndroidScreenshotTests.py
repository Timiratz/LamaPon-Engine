import binascii
import struct
import unittest
import zlib
from types import SimpleNamespace
from unittest.mock import patch

from tests import AndroidEmulatorSmoke
from tools.check_android_screenshot import count_green_marker, decode_android_png


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", binascii.crc32(kind + payload) & 0xffffffff)


def paeth(left: int, above: int, upper_left: int) -> int:
    estimate = left + above - upper_left
    distances = (abs(estimate - left), abs(estimate - above), abs(estimate - upper_left))
    return (left, above, upper_left)[distances.index(min(distances))]


def encode_row(row: bytes, previous: bytes, channels: int, filter_type: int) -> bytes:
    encoded = bytearray(len(row))
    for index, value in enumerate(row):
        left = row[index - channels] if index >= channels else 0
        above = previous[index] if previous else 0
        upper_left = previous[index - channels] if previous and index >= channels else 0
        if filter_type == 1:
            predictor = left
        elif filter_type == 2:
            predictor = above
        elif filter_type == 3:
            predictor = (left + above) // 2
        elif filter_type == 4:
            predictor = paeth(left, above, upper_left)
        else:
            predictor = 0
        encoded[index] = (value - predictor) & 0xff
    return bytes((filter_type,)) + encoded


def make_png(rows: list[bytes], width: int, channels: int = 4) -> bytes:
    color_type = 6 if channels == 4 else 2
    ihdr = struct.pack(">IIBBBBB", width, len(rows), 8, color_type, 0, 0, 0)
    scanlines = bytearray()
    previous = b""
    for index, row in enumerate(rows):
        scanlines.extend(encode_row(row, previous, channels, index % 5))
        previous = row
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", ihdr)
            + png_chunk(b"IDAT", zlib.compress(scanlines)) + png_chunk(b"IEND", b""))


def screenshot_header(width: int, height: int) -> bytes:
    screenshot = bytearray(24)
    screenshot[:8] = b"\x89PNG\r\n\x1a\n"
    screenshot[16:24] = struct.pack(">II", width, height)
    return bytes(screenshot)


class AndroidScreenshotTests(unittest.TestCase):
    def test_closes_only_the_focused_emulator_launcher_anr(self):
        focused = ("mCurrentFocus=Window{123 u0 Application Not Responding: "
                   + AndroidEmulatorSmoke.EMULATOR_LAUNCHER + "}")
        with patch.object(AndroidEmulatorSmoke, "adb",
                          return_value=SimpleNamespace(stdout=focused)) as adb:
            self.assertTrue(AndroidEmulatorSmoke.dismiss_emulator_launcher_anr())
        self.assertEqual(adb.call_args.args,
                         ("shell", "am", "force-stop", AndroidEmulatorSmoke.EMULATOR_LAUNCHER))

    def test_does_not_dismiss_game_anr_or_unfocused_launcher_window(self):
        for focused in (
                "mCurrentFocus=Window{123 u0 Application Not Responding: "
                + AndroidEmulatorSmoke.PACKAGE + "}",
                "mCurrentFocus=Window{123 u0 " + AndroidEmulatorSmoke.PACKAGE + "}\n"
                "Window #1 Application Not Responding: " + AndroidEmulatorSmoke.EMULATOR_LAUNCHER + "}"):
            with self.subTest(focused=focused), patch.object(
                    AndroidEmulatorSmoke, "adb", return_value=SimpleNamespace(stdout=focused)) as adb:
                self.assertFalse(AndroidEmulatorSmoke.dismiss_emulator_launcher_anr())
                self.assertEqual(adb.call_count, 1)

    def test_prepares_interactive_emulator_before_game_launch(self):
        with patch.object(AndroidEmulatorSmoke, "adb",
                          return_value=SimpleNamespace(stdout="1\n")) as adb:
            AndroidEmulatorSmoke.prepare_headless_emulator()
        commands = [call.args for call in adb.call_args_list]
        self.assertLess(commands.index(("shell", "input", "keyevent", "KEYCODE_WAKEUP")),
                        commands.index(("shell", "wm", "dismiss-keyguard")))
        self.assertIn(("shell", "svc", "power", "stayon", "true"), commands)

    def test_diagnostic_failure_preserves_original_smoke_error(self):
        original = RuntimeError("scene never rendered")
        with patch.object(AndroidEmulatorSmoke, "run_smoke", side_effect=original), \
                patch.object(AndroidEmulatorSmoke, "collect_failure_diagnostics",
                             side_effect=OSError("disk full")), \
                patch.object(AndroidEmulatorSmoke, "adb") as adb:
            with self.assertRaises(RuntimeError) as caught:
                AndroidEmulatorSmoke.main()
        self.assertIs(caught.exception, original)
        adb.assert_called_once_with("shell", "wm", "size", "reset", check=False)

    def test_decodes_all_png_filters_and_counts_rgba_green_marker(self):
        rows = []
        for y in range(10):
            row = bytearray()
            for x in range(10):
                row.extend((0, 220, 0, 255) if 2 <= x < 8 and 2 <= y < 8
                           else (20, 20, 20, 255))
            rows.append(bytes(row))
        screenshot = make_png(rows, width=10)
        width, height, channels, decoded = decode_android_png(screenshot)
        self.assertEqual((width, height, channels), (10, 10, 4))
        self.assertEqual(decoded, b"".join(rows))
        self.assertEqual(count_green_marker(screenshot),
                         {"width": 10, "height": 10, "greenMarkerPixels": 36})

    def test_decodes_rgb_screenshot(self):
        row = bytes((0, 200, 0, 4, 5, 6))
        result = make_png([row], width=2, channels=3)
        self.assertEqual(decode_android_png(result), (2, 1, 3, row))

    def test_rejects_corrupt_chunk_crc(self):
        screenshot = bytearray(make_png([bytes((0, 200, 0, 255))], width=1))
        screenshot[29] ^= 1
        with self.assertRaisesRegex(ValueError, "corrupt PNG chunk"):
            decode_android_png(bytes(screenshot))

    def test_rejects_truncated_and_non_png_data(self):
        with self.assertRaisesRegex(ValueError, "not a PNG"):
            decode_android_png(b"not a screenshot")
        with self.assertRaisesRegex(ValueError, "truncated PNG chunk"):
            decode_android_png(b"\x89PNG\r\n\x1a\n\x00")

    def test_waits_for_scene_marker_in_android_screenshot(self):
        screenshot = screenshot_header(720, 1560)
        with patch.object(AndroidEmulatorSmoke, "dismiss_emulator_launcher_anr"), \
                patch.object(AndroidEmulatorSmoke, "screenshot_render",
                          return_value=(screenshot, {"greenMarkerPixels": 1000})):
            captured, rendered = AndroidEmulatorSmoke.wait_for_scene_screenshot("startup")
        self.assertEqual(captured, screenshot)
        self.assertEqual(rendered["greenMarkerPixels"], 1000)

    def test_waits_for_new_dimensions_after_android_display_resize(self):
        original = screenshot_header(720, 1560)
        resized = screenshot_header(600, 1300)
        with patch.object(AndroidEmulatorSmoke, "dismiss_emulator_launcher_anr"), \
                patch.object(AndroidEmulatorSmoke, "screenshot_render", side_effect=(
                (original, {"greenMarkerPixels": 12000}),
                (resized, {"greenMarkerPixels": 8000}))) as capture:
            captured, rendered = AndroidEmulatorSmoke.wait_for_scene_screenshot(
                "display resize", (720, 1560))
        self.assertEqual(capture.call_count, 2)
        self.assertEqual(captured, resized)
        self.assertEqual(rendered["greenMarkerPixels"], 8000)


if __name__ == "__main__":
    unittest.main()
