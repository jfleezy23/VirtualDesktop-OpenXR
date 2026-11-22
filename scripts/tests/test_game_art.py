import base64
from pathlib import Path
import struct
import sys
import tempfile
import tkinter as tk
import unittest
import zlib


# 40 x 20 solid orange JPEG.
JPEG = base64.b64decode(
    "/9j/4AAQSkZJRgABAQEAYABgAAD/2wBDAAMCAgMCAgMDAwMEAwMEBQgFBQQEBQoHBwYIDAoMDAsKCwsNDhIQDQ4RDgsLEBYQERMUFRUVDA8XGB"
    "YUGBIUFRT/2wBDAQMEBAUEBQkFBQkUDQsNFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBT/wAARCAAU"
    "ACgDASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhBy"
    "JxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKT"
    "lJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAA"
    "AAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRom"
    "JygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExc"
    "bHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwDj6KKK/Bz+7QooooAKKKKACiiigAooooAKKKKAP//Z"
)


def png(width, height):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    opaque = b"\0" + bytes((234, 62, 30, 255)) * width
    transparent = b"\0" + bytes((234, 62, 30, 0)) * width
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(opaque * (height - height // 2) + transparent * (height // 2)))
            + chunk(b"IEND", b""))


@unittest.skipUnless(sys.platform == "win32", "Requires Windows GDI+")
class GameArtTests(unittest.TestCase):
    def setUp(self):
        self.root = tk.Tk()
        self.root.withdraw()
        self.addCleanup(self.root.destroy)
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.folder = Path(folder.name)

    def cover(self, path):
        from vdxr_game_art import cover_photo

        return cover_photo(self.root, path, (48, 32), "#102030")

    def test_jpeg_keeps_aspect_ratio_and_rgb_channel_order(self):
        path = self.folder / "cover.jpg"
        path.write_bytes(JPEG)
        image = self.cover(path)
        self.assertIsNotNone(image)
        self.assertEqual((image.width(), image.height()), (48, 32))
        self.assertEqual(image.get(24, 1), (16, 32, 48))
        self.assertEqual(image.get(24, 30), (16, 32, 48))
        for actual, expected in zip(image.get(24, 16), (234, 62, 30)):
            self.assertAlmostEqual(actual, expected, delta=5)

    def test_png_preserves_row_order_and_blends_transparency_into_matte(self):
        path = self.folder / "cover.png"
        path.write_bytes(png(4, 8))
        image = self.cover(path)
        self.assertIsNotNone(image)
        self.assertEqual(image.get(3, 8), (16, 32, 48))
        self.assertEqual(image.get(44, 8), (16, 32, 48))
        self.assertEqual(image.get(24, 8), (234, 62, 30))
        self.assertEqual(image.get(24, 24), (16, 32, 48))

    def test_missing_and_corrupt_artwork_use_placeholder(self):
        path = self.folder / "cover.jpg"
        for name, data in (("missing", None), ("corrupt", b"corrupt artwork"), ("truncated", JPEG[:200])):
            with self.subTest(name=name):
                if data is not None:
                    path.write_bytes(data)
                self.assertIsNone(self.cover(path))
        self.assertIsNone(self.cover(None))

    def test_rejects_truncated_png_and_corrupt_chunk_checksum(self):
        path = self.folder / "cover.png"
        original = png(4, 8)
        changed = bytearray(original)
        changed[-13] ^= 1  # Corrupt the IDAT checksum before IEND.
        for name, data in (("truncated", original[:-12]), ("invalid checksum", changed)):
            with self.subTest(name=name):
                path.write_bytes(data)
                self.assertIsNone(self.cover(path))

    def test_rejects_artwork_over_input_limits(self):
        path = self.folder / "cover.png"
        path.write_bytes(png(9000, 1))
        self.assertIsNone(self.cover(path))
        with path.open("wb") as image_file:
            image_file.write(png(4, 8))
            image_file.truncate(25 * 1024 * 1024)
        self.assertIsNone(self.cover(path))
