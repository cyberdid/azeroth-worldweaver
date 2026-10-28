import pathlib
import runpy
import struct
import tempfile
import threading
import time
import json
import unittest
from concurrent.futures import ThreadPoolExecutor
from unittest import mock


CAPTURE_PATH = pathlib.Path(__file__).resolve().parents[1] / "tools" / "wow-agent" / "wow_window_capture.py"
CAPTURE = runpy.run_path(str(CAPTURE_PATH), run_name="wow_window_capture_under_test")
CLAUDE_PATH = pathlib.Path(__file__).resolve().parents[1] / "tools" / "wow-agent" / "claude-bridge.py"
CLAUDE = runpy.run_path(str(CLAUDE_PATH), run_name="claude_bridge_under_test")


class XwdDecodeTests(unittest.TestCase):
    def test_padded_little_endian_rgb_uses_pillow_raw_decoder(self):
        width, height, stride = 2, 2, 8
        header = [0] * 25
        header[0] = 100  # header_size
        header[1] = 7  # XWD version
        header[2] = 2  # ZPixmap
        header[3] = 24  # depth
        header[4] = width
        header[5] = height
        header[6] = 0  # xoffset
        header[7] = 0  # little-endian pixels
        header[10] = 32  # bitmap_pad
        header[11] = 24  # bits_per_pixel
        header[12] = stride
        header[13] = 5  # DirectColor
        header[14] = 0x00FF0000
        header[15] = 0x0000FF00
        header[16] = 0x000000FF
        header[17] = 8  # bits_per_rgb
        header[18] = 256  # colormap_entries
        header[20] = width
        header[21] = height
        pixel_rows = bytes((
            0, 0, 255, 0, 255, 0, 17, 17,       # red, green, row padding
            255, 0, 0, 255, 255, 255, 29, 29,   # blue, white, row padding
        ))

        with tempfile.TemporaryDirectory() as directory:
            xwd_path = pathlib.Path(directory) / "frame.xwd"
            xwd_path.write_bytes(struct.pack(">25I", *header) + pixel_rows)
            with mock.patch.object(CAPTURE["Image"], "frombytes", wraps=CAPTURE["Image"].frombytes) as decode:
                image = CAPTURE["_decode_xwd"](str(xwd_path))

        self.assertEqual(image.size, (width, height))
        self.assertEqual(image.getpixel((0, 0)), (255, 0, 0))
        self.assertEqual(image.getpixel((1, 0)), (0, 255, 0))
        self.assertEqual(image.getpixel((0, 1)), (0, 0, 255))
        self.assertEqual(image.getpixel((1, 1)), (255, 255, 255))
        self.assertEqual(decode.call_args.args[3:], ("raw", "BGR", stride, 1))


class VisualRequestTests(unittest.TestCase):
    def test_explicit_visual_requests_are_detected_in_ukrainian_and_english(self):
        detect = CAPTURE.get("is_visual_request")
        self.assertTrue(callable(detect), "the shared capture helper must expose request detection")
        for text in ("Кодекс, що бачиш на екрані?", "Глянь на гру", "What do you see?"):
            with self.subTest(text=text):
                self.assertTrue(detect(text))

    def test_ordinary_game_questions_do_not_request_a_screenshot(self):
        detect = CAPTURE.get("is_visual_request")
        self.assertTrue(callable(detect), "the shared capture helper must expose request detection")
        for text in ("Давайте порибалимо", "Що відбулось у бою?", "Follow me"):
            with self.subTest(text=text):
                self.assertFalse(detect(text))

    def test_explicitly_rejected_screenshots_are_not_captured(self):
        detect = CAPTURE.get("is_visual_request")
        self.assertTrue(callable(detect), "the shared capture helper must expose request detection")
        for text in ("Не роби скріншот, будь ласка.", "No screenshot, please."):
            with self.subTest(text=text):
                self.assertFalse(detect(text))

    def test_nonvisual_message_skips_capture_and_returns_no_image(self):
        capture_for_request = CAPTURE.get("capture_for_request")
        self.assertTrue(callable(capture_for_request), "the helper must gate captures by user intent")
        namespace = capture_for_request.__globals__
        original = namespace["capture_game_window"]

        def unexpected_capture(*_args, **_kwargs):
            self.fail("nonvisual message invoked the screenshot capture")

        namespace["capture_game_window"] = unexpected_capture
        try:
            with tempfile.TemporaryDirectory() as directory:
                result = capture_for_request("Давайте порибалимо", None, directory)
        finally:
            namespace["capture_game_window"] = original

        self.assertEqual(result["available"], False)
        self.assertEqual(result["requested"], False)

    def test_claude_preserves_no_capture_status_for_nonvisual_messages(self):
        visual_context = CLAUDE["visual_context"]
        with mock.patch.object(visual_context.__globals__["WINDOW_CAPTURE"], "capture_for_request",
                               return_value={"available": False, "requested": False,
                                             "reason": "No explicit visual request."}):
            result = visual_context({"group_id": "1", "snapshot_available": True}, "Давайте порибалимо")

        self.assertFalse(json.loads(result)["requested"])


class CaptureSerializationTests(unittest.TestCase):
    def test_concurrent_agents_do_not_capture_the_window_at_the_same_time(self):
        namespace = CAPTURE["capture_game_window"].__globals__
        original = namespace["_capture_game_window"]
        state_lock = threading.Lock()
        active = 0
        maximum_active = 0

        def slow_capture(_snapshot_path, _output_dir, _min_interval_seconds):
            nonlocal active, maximum_active
            with state_lock:
                active += 1
                maximum_active = max(maximum_active, active)
            time.sleep(0.05)
            with state_lock:
                active -= 1
            return {"available": False, "reason": "controlled test capture"}

        namespace["_capture_game_window"] = slow_capture
        try:
            with tempfile.TemporaryDirectory() as directory:
                with ThreadPoolExecutor(max_workers=2) as pool:
                    futures = [pool.submit(CAPTURE["capture_game_window"], None, directory)
                               for _ in range(2)]
                    for future in futures:
                        future.result()
        finally:
            namespace["_capture_game_window"] = original

        self.assertEqual(maximum_active, 1)


if __name__ == "__main__":
    unittest.main()
