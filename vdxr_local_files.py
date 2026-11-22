"""Bounded local filesystem reads for elevated Controls metadata and artwork."""

from contextlib import ExitStack, contextmanager
import ctypes
from ctypes import wintypes
from fnmatch import fnmatch
from functools import cache
import ntpath
import os
from pathlib import Path
import re
import struct
import sys

if sys.platform == "win32":
    import msvcrt


_LOCAL_DRIVES = {2, 3, 5, 6}  # Removable, fixed, optical, and RAM disk.
_FILE_SHARE_READ = 1
_FILE_LIST_DIRECTORY_OR_READ_DATA = 1
_FILE_READ_ATTRIBUTES = 0x80
_SYNCHRONIZE = 0x100000
_READ_ACCESS = _FILE_LIST_DIRECTORY_OR_READ_DATA | _FILE_READ_ATTRIBUTES | _SYNCHRONIZE
_FILE_FLAG_OPEN_REPARSE_POINT = 0x00200000
_FILE_FLAG_BACKUP_SEMANTICS = 0x02000000
_OPEN_EXISTING = 3
_FILE_DIRECTORY_FILE = 1
_FILE_NON_DIRECTORY_FILE = 0x40
_FILE_SYNCHRONOUS_IO_NONALERT = 0x20
_MARKED_DIRECTORY = 0x10
_MARKED_REPARSE_POINT = 0x400
_FILE_ATTRIBUTE_TAG_INFO = 9
_FILE_NAMES_INFORMATION = 12
_FILE_TYPE_DISK = 1
_OBJ_CASE_INSENSITIVE = 0x40
_OBJ_DONT_REPARSE = 0x1000
_INVALID_HANDLE = ctypes.c_void_p(-1).value
_NAME_RECORD = struct.Struct("<III")


class _UnicodeString(ctypes.Structure):
    _fields_ = [("length", wintypes.USHORT), ("maximum_length", wintypes.USHORT),
                ("buffer", ctypes.c_void_p)]


class _ObjectAttributes(ctypes.Structure):
    _fields_ = [("length", wintypes.ULONG), ("root_directory", wintypes.HANDLE),
                ("object_name", ctypes.POINTER(_UnicodeString)), ("attributes", wintypes.ULONG),
                ("security_descriptor", ctypes.c_void_p), ("security_qos", ctypes.c_void_p)]


class _IoStatusBlock(ctypes.Structure):
    _fields_ = [("status_or_pointer", ctypes.c_void_p), ("information", ctypes.c_size_t)]


@cache
def _windows_files():
    kernel = ctypes.WinDLL("kernel32.dll")
    native = ctypes.WinDLL("ntdll.dll")
    handle, dword, boolean, pointer = wintypes.HANDLE, wintypes.DWORD, wintypes.BOOL, ctypes.c_void_p
    signatures = (
        (kernel.GetDriveTypeW, (wintypes.LPCWSTR,), wintypes.UINT),
        (kernel.GetVolumeNameForVolumeMountPointW, (wintypes.LPCWSTR, wintypes.LPWSTR, dword), boolean),
        (kernel.CreateFileW, (wintypes.LPCWSTR, dword, dword, pointer, dword, dword, handle), handle),
        (kernel.GetFileType, (handle,), dword),
        (kernel.GetFileInformationByHandleEx, (handle, ctypes.c_int, pointer, dword), boolean),
        (kernel.GetFileSizeEx, (handle, ctypes.POINTER(ctypes.c_longlong)), boolean),
        (kernel.CloseHandle, (handle,), boolean),
        (native.NtOpenFile, (ctypes.POINTER(handle), dword, ctypes.POINTER(_ObjectAttributes),
                             ctypes.POINTER(_IoStatusBlock), dword, dword), wintypes.LONG),
        (native.NtQueryDirectoryFile, (handle, handle, pointer, pointer, ctypes.POINTER(_IoStatusBlock),
                                        pointer, dword, ctypes.c_int, ctypes.c_ubyte,
                                        ctypes.POINTER(_UnicodeString), ctypes.c_ubyte), wintypes.LONG),
    )
    for function, arguments, result in signatures:
        function.argtypes = arguments
        function.restype = result
    return kernel, native


def _ordinary_name(name):
    return (name not in ("", ".", "..") and not name.endswith((".", " "))
            and not ntpath.isreserved(name))


def _verified_handle(handle, *, directory):
    if handle in (None, _INVALID_HANDLE):
        return None
    kernel, _ = _windows_files()
    # FileAttributeTagInfo returns two DWORDs, file attributes and the reparse tag.
    attributes = (wintypes.DWORD * 2)()
    if (kernel.GetFileType(handle) == _FILE_TYPE_DISK
            and kernel.GetFileInformationByHandleEx(handle, _FILE_ATTRIBUTE_TAG_INFO,
                                                    attributes, ctypes.sizeof(attributes))
            and not attributes[0] & _MARKED_REPARSE_POINT
            and bool(attributes[0] & _MARKED_DIRECTORY) == directory):
        return handle
    kernel.CloseHandle(handle)
    return None


def _open_volume_root(anchor):
    kernel, _ = _windows_files()
    if kernel.GetDriveTypeW(anchor) not in _LOCAL_DRIVES:
        return None
    volume = ctypes.create_unicode_buffer(64)
    if not kernel.GetVolumeNameForVolumeMountPointW(anchor, volume, len(volume)):
        return None
    # Resolve the drive letter once, then use the physical volume and handles.
    root = kernel.CreateFileW(volume.value, _READ_ACCESS, _FILE_SHARE_READ, None, _OPEN_EXISTING,
                              _FILE_FLAG_BACKUP_SEMANTICS | _FILE_FLAG_OPEN_REPARSE_POINT, None)
    return _verified_handle(root, directory=True)


def _open_child(parent, name, *, directory):
    if not _ordinary_name(name):
        return None
    # Each call receives one name relative to the existing directory handle.
    # Even if a file system allows attribute-only updates while shared, name
    # resolution cannot follow a newly installed reparse point.
    encoded = name.encode("utf-16-le", errors="surrogatepass")
    if len(encoded) > 65532:
        return None
    filename = ctypes.create_string_buffer(encoded, len(encoded) + 2)
    object_name = _UnicodeString(len(encoded), ctypes.sizeof(filename), ctypes.addressof(filename))
    attributes = _ObjectAttributes(ctypes.sizeof(_ObjectAttributes), parent, ctypes.pointer(object_name),
                                   _OBJ_CASE_INSENSITIVE | _OBJ_DONT_REPARSE, None, None)
    options = (_FILE_DIRECTORY_FILE if directory else _FILE_NON_DIRECTORY_FILE)
    options |= _FILE_FLAG_OPEN_REPARSE_POINT | _FILE_SYNCHRONOUS_IO_NONALERT
    _, native = _windows_files()
    handle = wintypes.HANDLE()
    status = _IoStatusBlock()
    if native.NtOpenFile(ctypes.byref(handle), _READ_ACCESS, ctypes.byref(attributes),
                         ctypes.byref(status), _FILE_SHARE_READ, options) != 0:
        return None
    return _verified_handle(handle.value, directory=directory)


class _LocalDirectory:
    def __init__(self, path, handle):
        self._path = path
        self._handle = handle

    def iterdir(self):
        """Enumerate the pinned object through its native handle."""
        _, native = _windows_files()
        buffer = ctypes.create_string_buffer(8192)
        restart = True
        while self._handle is not None:
            result = _IoStatusBlock()
            status = native.NtQueryDirectoryFile(self._handle, None, None, None, ctypes.byref(result),
                                                 buffer, len(buffer), _FILE_NAMES_INFORMATION, False, None, restart)
            if status != 0 or not 0 < result.information <= len(buffer):
                return
            restart = False
            records = buffer.raw[:result.information]
            offset = 0
            while offset + _NAME_RECORD.size <= len(records):
                next_offset, _, length = _NAME_RECORD.unpack_from(records, offset)
                start = offset + _NAME_RECORD.size
                end = start + length
                if length % 2 or end > len(records):
                    return
                name = records[start:end].decode("utf-16-le", errors="surrogatepass")
                if _ordinary_name(name):
                    yield self._path / name
                if next_offset == 0:
                    break
                if offset + next_offset < end:
                    return
                offset += next_offset

    def glob(self, pattern):
        """Yield immediate children whose names match a shell wildcard."""
        return (child for child in self.iterdir() if fnmatch(child.name, pattern))


@contextmanager
def local_directory(path: Path):
    """Yield a handle-backed directory with glob/iterdir, or None."""
    if (sys.platform != "win32" or not path.is_absolute() or not re.fullmatch(r"[A-Za-z]:", path.drive)
            or not all(_ordinary_name(part) for part in path.parts[1:])):
        yield None
        return
    kernel, _ = _windows_files()
    with ExitStack() as handles:
        handle = _open_volume_root(path.anchor)
        for part in path.parts[1:]:
            if handle is None:
                break
            handles.callback(kernel.CloseHandle, handle)
            handle = _open_child(handle, part, directory=True)
        if handle is None:
            yield None
            return
        handles.callback(kernel.CloseHandle, handle)
        directory = _LocalDirectory(path, handle)
        try:
            yield directory
        finally:
            directory._handle = None


def _file_size(kernel, handle):
    size = ctypes.c_longlong()
    return size.value if kernel.GetFileSizeEx(handle, ctypes.byref(size)) else -1


def is_local_file(path: Path) -> bool:
    """Return whether path names readable, nonempty local content."""
    with local_directory(path.parent) as parent:
        if parent is None:
            return False
        handle = _open_child(parent._handle, path.name, directory=False)
        if handle is None:
            return False
        kernel, _ = _windows_files()
        try:
            return _file_size(kernel, handle) > 0
        finally:
            kernel.CloseHandle(handle)


def read_local_bytes(path: Path, limit: int) -> bytes | None:
    """Read a regular file through the same handle that was checked."""
    if limit < 0:
        return None
    with local_directory(path.parent) as parent:
        if parent is None:
            return None
        handle = _open_child(parent._handle, path.name, directory=False)
        if handle is None:
            return None
        kernel, _ = _windows_files()
        try:
            if not 0 <= _file_size(kernel, handle) <= limit:
                return None
            fd = msvcrt.open_osfhandle(handle, os.O_RDONLY | os.O_BINARY | os.O_NOINHERIT)
            handle = None  # The CRT descriptor now owns the handle.
            try:
                stream = os.fdopen(fd, "rb")
            except OSError:
                os.close(fd)
                raise
            with stream:
                data = stream.read(limit + 1)
            return data if len(data) <= limit else None
        except (OSError, OverflowError):
            return None
        finally:
            if handle is not None:
                kernel.CloseHandle(handle)
