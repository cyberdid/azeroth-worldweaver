import pathlib
import runpy
import struct
import tempfile
import unittest
from unittest import mock


CAPTURE_PATH = pathlib.Path(__file__).resolve().parents[1] / "tools" / "wow-agent" / "wow_window_capture.py"
CAPTURE = runpy.run_path(str(CAPTURE_PATH), run_name="wow_window_capture_under_test")


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


if __name__ == "__main__":
    unittest.main()
