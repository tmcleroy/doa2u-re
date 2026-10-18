# SPDX-License-Identifier: GPL-3.0-or-later
import struct
import unittest

from dedication_card import morton_tables, paletted_textures


class TextureTests(unittest.TestCase):
    def test_swizzle_interleaves_x_then_y(self):
        xs, ys = morton_tables(4, 2)
        self.assertEqual([[xs[x] | ys[y] for x in range(4)] for y in range(2)],
                         [[0, 1, 4, 5], [2, 3, 6, 7]])

    def test_paletted_texture_unswizzles_through_following_palette(self):
        xs, ys = morton_tables(4, 2)
        texels = bytearray(8)
        for y in range(2):
            for x in range(4):
                texels[xs[x] | ys[y]] = y * 4 + x
        palette = b"".join(bytes((i, 10 + i, 20 + i, 255)) for i in range(32))  # BGRA
        header = 12 + 20 + 12 + 4
        blob = (b"XPR0" + struct.pack("<II", header + 8 + len(palette), header)
                + struct.pack("<5I", 4 << 16, 0, 0, 0x0B << 8 | 2 << 20 | 1 << 24, 0)
                + struct.pack("<3I", 3 << 16 | 3 << 30, 8, 0)
                + struct.pack("<I", 0xFFFFFFFF) + bytes(texels) + palette)
        self.assertEqual(list(paletted_textures(blob)), [
            (4, 2, b"".join(bytes((20 + i, 10 + i, i, 255)) for i in range(8)))])


if __name__ == "__main__":
    unittest.main()
