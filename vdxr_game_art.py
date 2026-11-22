"""Render read-only game artwork as bounded Tk thumbnails using Windows GDI+."""

import ctypes
from ctypes import wintypes
from functools import cache
from os import PathLike
from pathlib import Path
import struct
import sys
import tkinter as tk

from vdxr_local_files import read_local_bytes
import zlib


_MAX_FILE_BYTES = 16 * 1024 * 1024
_MAX_SOURCE_DIMENSION = 8192
_MAX_SOURCE_PIXELS = 16 * 1024 * 1024
_MAX_THUMBNAIL_DIMENSION = 1024
_PIXEL_FORMAT_32BPP_ARGB = 0x26200A
_UNIT_PIXEL = 2
_INTERPOLATION_HIGH_QUALITY_BILINEAR = 6
_FLUSH_SYNC = 1
_SYSTEM32_ONLY = 0x800
_JPEG_FRAME_MARKERS = frozenset(range(0xC0, 0xD0)) - {0xC4, 0xC8, 0xCC}


class _GdiStartupInput(ctypes.Structure):
    _fields_ = [
        ("version", wintypes.UINT),
        ("debug_callback", ctypes.c_void_p),
        ("suppress_background_thread", wintypes.BOOL),
        ("suppress_external_codecs", wintypes.BOOL),
    ]


class _DecodeError(Exception):
    pass


@cache
def _windows_graphics():
    """Bind only the system DLLs. Native objects are scoped to one decode."""
    gdip = ctypes.WinDLL("gdiplus.dll", winmode=_SYSTEM32_ONLY)
    shell = ctypes.WinDLL("shlwapi.dll", winmode=_SYSTEM32_ONLY)
    ptr = ctypes.c_void_p
    int32 = ctypes.c_int
    uint = wintypes.UINT
    out_ptr = ctypes.POINTER(ptr)
    signatures = {
        "GdiplusStartup": (ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(_GdiStartupInput), ptr),
        "GdiplusShutdown": (ctypes.c_size_t,),
        "GdipCreateBitmapFromStream": (ptr, out_ptr),
        "GdipGetImageWidth": (ptr, ctypes.POINTER(uint)),
        "GdipGetImageHeight": (ptr, ctypes.POINTER(uint)),
        "GdipCreateBitmapFromScan0": (int32, int32, int32, int32, ptr, out_ptr),
        "GdipGetImageGraphicsContext": (ptr, out_ptr),
        "GdipGraphicsClear": (ptr, wintypes.DWORD),
        "GdipSetInterpolationMode": (ptr, int32),
        "GdipDrawImageRectRectI": (ptr, ptr) + (int32,) * 9 + (ptr, ptr, ptr),
        "GdipFlush": (ptr, int32),
        "GdipDeleteGraphics": (ptr,),
        "GdipDisposeImage": (ptr,),
    }
    for name, args in signatures.items():
        function = getattr(gdip, name)
        function.argtypes = args
        function.restype = None if name == "GdiplusShutdown" else int32
    create_stream = shell.SHCreateMemStream
    create_stream.argtypes = (ctypes.c_char_p, uint)
    create_stream.restype = ptr
    return gdip, create_stream


def _png_size(data):
    # GDI+ can accept incomplete PNGs and ignore invalid chunk checksums.
    view = memoryview(data)
    offset = 8
    has_pixels = False
    while offset + 12 <= len(data):
        length, kind = struct.unpack_from(">I4s", data, offset)
        checksum = offset + 8 + length
        end = checksum + 4
        if end > len(data) or zlib.crc32(view[offset + 4:checksum]) != struct.unpack_from(">I", data, checksum)[0]:
            break
        if kind == b"IEND":
            if length == 0 and end == len(data) and has_pixels:
                return struct.unpack_from(">II", data, 16)
            break
        has_pixels |= kind == b"IDAT"
        offset = end
    raise _DecodeError


def _source_size(data):
    """Read dimensions before handing compressed data to the native decoder."""
    if data.startswith(b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR") and len(data) >= 33:
        return _png_size(data)
    if data.startswith(b"\xff\xd8"):
        index = 2
        while index + 3 < len(data) and data[index] == 0xFF:
            while index < len(data) and data[index] == 0xFF:
                index += 1
            if index + 2 >= len(data) or data[index] in (0xDA, 0xD9):
                break  # A frame header must precede the first scan or end marker.
            marker = data[index]
            length = int.from_bytes(data[index + 1:index + 3], "big")
            end = index + 1 + length
            if length < 2 or end > len(data):
                break
            if marker in _JPEG_FRAME_MARKERS and length >= 8 and data.rfind(b"\xff\xd9") >= end:
                height, width = struct.unpack_from(">HH", data, index + 4)
                return width, height
            index = end
    raise _DecodeError


def _check(status):
    if status:
        raise _DecodeError


def _render(data, source_size, size, matte):
    gdip, create_stream = _windows_graphics()
    width, height = size
    source_width, source_height = source_size
    scale = min(width / source_width, height / source_height)
    draw_width = max(1, round(scale * source_width))
    draw_height = max(1, round(scale * source_height))
    x = (width - draw_width) // 2
    y = (height - draw_height) // 2

    token = ctypes.c_size_t()
    source_bitmap = ctypes.c_void_p()
    thumbnail = ctypes.c_void_p()
    graphics = ctypes.c_void_p()
    # Place the bitmap into our own top-down, tightly packed BGRA rows.
    stride = width * 4
    pixels = ctypes.create_string_buffer(stride * height)
    _check(gdip.GdiplusStartup(ctypes.byref(token), ctypes.byref(_GdiStartupInput(1, None, False, True)), None))
    try:
        stream = create_stream(data, len(data))
        if not stream:
            raise _DecodeError
        try:
            _check(gdip.GdipCreateBitmapFromStream(stream, ctypes.byref(source_bitmap)))
            native_width, native_height = wintypes.UINT(), wintypes.UINT()
            _check(gdip.GdipGetImageWidth(source_bitmap, ctypes.byref(native_width)))
            _check(gdip.GdipGetImageHeight(source_bitmap, ctypes.byref(native_height)))
            if (native_width.value, native_height.value) != source_size:
                raise _DecodeError
            _check(gdip.GdipCreateBitmapFromScan0(width, height, stride, _PIXEL_FORMAT_32BPP_ARGB,
                                                  pixels, ctypes.byref(thumbnail)))
            _check(gdip.GdipGetImageGraphicsContext(thumbnail, ctypes.byref(graphics)))
            _check(gdip.GdipGraphicsClear(graphics, matte))
            _check(gdip.GdipSetInterpolationMode(graphics, _INTERPOLATION_HIGH_QUALITY_BILINEAR))
            _check(gdip.GdipDrawImageRectRectI(graphics, source_bitmap, x, y, draw_width, draw_height,
                                              0, 0, source_width, source_height, _UNIT_PIXEL, None, None, None))
            _check(gdip.GdipFlush(graphics, _FLUSH_SYNC))
        finally:
            if graphics:
                gdip.GdipDeleteGraphics(graphics)
            if thumbnail:
                gdip.GdipDisposeImage(thumbnail)
            if source_bitmap:
                gdip.GdipDisposeImage(source_bitmap)
            # IUnknown::Release is entry 2 of the IStream vtable. GDI+ needs
            # the stream to stay alive until the source bitmap is disposed.
            vtable = ctypes.cast(stream, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
            release = ctypes.WINFUNCTYPE(wintypes.ULONG, ctypes.c_void_p)(vtable[2])
            release(stream)
    finally:
        gdip.GdiplusShutdown(token)

    bgra = pixels.raw
    rgb = bytearray(width * height * 3)
    rgb[0::3] = bgra[2::4]
    rgb[1::3] = bgra[1::4]
    rgb[2::3] = bgra[0::4]
    return f"P6\n{width} {height}\n255\n".encode("ascii") + rgb


def cover_photo(parent: tk.Misc, path: str | PathLike[str] | None, size: tuple[int, int],
                background: str) -> tk.PhotoImage | None:
    """Fit a local JPEG or PNG into a matte-backed Tk image on the Tk thread."""
    if path is None or sys.platform != "win32" or any(not 0 < n <= _MAX_THUMBNAIL_DIMENSION for n in size):
        return None
    try:
        data = read_local_bytes(Path(path), _MAX_FILE_BYTES)
        if not data:
            return None
        width, height = source_size = _source_size(data)
        if (not 0 < width <= _MAX_SOURCE_DIMENSION or not 0 < height <= _MAX_SOURCE_DIMENSION
                or width * height > _MAX_SOURCE_PIXELS):
            return None
        red, green, blue = (channel >> 8 for channel in parent.winfo_rgb(background))
        matte = 0xFF000000 | red << 16 | green << 8 | blue
        return tk.PhotoImage(master=parent, data=_render(data, source_size, size, matte), format="PPM")
    except (OSError, ValueError, tk.TclError, _DecodeError):
        return None
