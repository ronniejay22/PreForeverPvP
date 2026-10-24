"""Minimal ctypes wrapper around StormLib (v9.40, built with -DSTORM_UNICODE=ON)
for reading and rewriting the client patch MPQs.

StormLib 9.x returns C++ bool (one byte) from most calls. Declaring those as
c_int reads garbage in the upper bytes, so a "false" can look true; every
bool-returning function here is declared c_bool.

The DLL comes from STORMLIB_DLL, default D:\\WoWServer\\tools\\StormLib.dll.
"""
from __future__ import annotations

import ctypes
import os
from ctypes import POINTER, byref, c_bool, c_char, c_char_p, c_uint32, c_uint64, c_void_p, c_wchar_p
from pathlib import Path

DWORD = c_uint32
HANDLE = c_void_p

SFILE_OPEN_FROM_MPQ = 0x00000000
STREAM_FLAG_READ_ONLY = 0x00000100
MPQ_FILE_COMPRESS = 0x00000200
MPQ_FILE_REPLACEEXISTING = 0x80000000
MPQ_COMPRESSION_ZLIB = 0x02
LANG_NEUTRAL = 0
INTERNAL_FILES = {"(listfile)", "(attributes)", "(signature)"}


class SFileFindData(ctypes.Structure):
    _fields_ = [
        ("cFileName", c_char * 260),
        ("szPlainName", c_void_p),
        ("dwHashIndex", DWORD),
        ("dwBlockIndex", DWORD),
        ("dwFileSize", DWORD),
        ("dwFileFlags", DWORD),
        ("dwCompSize", DWORD),
        ("dwFileTimeLo", DWORD),
        ("dwFileTimeHi", DWORD),
        ("lcLocale", DWORD),
    ]


def _load(path: str | os.PathLike[str] | None) -> ctypes.WinDLL:
    dll = ctypes.WinDLL(str(path or os.environ.get("STORMLIB_DLL", r"D:\WoWServer\tools\StormLib.dll")),
                        use_last_error=True)
    sig = {
        "SFileOpenArchive": (c_bool, [c_wchar_p, DWORD, DWORD, POINTER(HANDLE)]),
        "SFileFlushArchive": (c_bool, [HANDLE]),
        "SFileCloseArchive": (c_bool, [HANDLE]),
        "SFileCompactArchive": (c_bool, [HANDLE, c_wchar_p, c_bool]),
        "SFileGetMaxFileCount": (DWORD, [HANDLE]),
        "SFileSetMaxFileCount": (c_bool, [HANDLE, DWORD]),
        "SFileHasFile": (c_bool, [HANDLE, c_char_p]),
        "SFileOpenFileEx": (c_bool, [HANDLE, c_char_p, DWORD, POINTER(HANDLE)]),
        "SFileGetFileSize": (DWORD, [HANDLE, POINTER(DWORD)]),
        "SFileReadFile": (c_bool, [HANDLE, c_void_p, DWORD, POINTER(DWORD), c_void_p]),
        "SFileCloseFile": (c_bool, [HANDLE]),
        "SFileFindFirstFile": (HANDLE, [HANDLE, c_char_p, POINTER(SFileFindData), c_wchar_p]),
        "SFileFindNextFile": (c_bool, [HANDLE, POINTER(SFileFindData)]),
        "SFileFindClose": (c_bool, [HANDLE]),
        "SFileCreateFile": (c_bool, [HANDLE, c_char_p, c_uint64, DWORD, DWORD, DWORD, POINTER(HANDLE)]),
        "SFileWriteFile": (c_bool, [HANDLE, c_void_p, DWORD, DWORD]),
        "SFileFinishFile": (c_bool, [HANDLE]),
        "SFileRemoveFile": (c_bool, [HANDLE, c_char_p, DWORD]),
    }
    for name, (restype, argtypes) in sig.items():
        fn = getattr(dll, name)
        fn.restype, fn.argtypes = restype, argtypes
    return dll


def _name(archived: str) -> bytes:
    # names inside an MPQ are narrow strings with backslashes
    return archived.replace("/", "\\").encode("latin-1")


class MPQ:
    def __init__(self, path: str | os.PathLike[str], writable: bool = False, dll: str | None = None):
        self.path = Path(path)
        self.dll = _load(dll)
        self.handle = HANDLE()
        flags = 0 if writable else STREAM_FLAG_READ_ONLY
        if not self.dll.SFileOpenArchive(str(self.path), 0, flags, byref(self.handle)):
            self._fail("SFileOpenArchive")
        self.writable = writable

    def _fail(self, what: str) -> None:
        raise OSError(ctypes.get_last_error(), f"{what} failed for {self.path}")

    def names(self, mask: str = "*") -> list[str]:
        data = SFileFindData()
        finder = self.dll.SFileFindFirstFile(self.handle, mask.encode("latin-1"), byref(data), None)
        if not finder:
            return []
        out = []
        try:
            while True:
                name = bytes(data.cFileName).split(b"\0", 1)[0].decode("latin-1")
                if name not in INTERNAL_FILES:
                    out.append(name)
                if not self.dll.SFileFindNextFile(finder, byref(data)):
                    break
        finally:
            self.dll.SFileFindClose(finder)
        return out

    def has(self, archived: str) -> bool:
        return bool(self.dll.SFileHasFile(self.handle, _name(archived)))

    def read(self, archived: str) -> bytes:
        f = HANDLE()
        if not self.dll.SFileOpenFileEx(self.handle, _name(archived), SFILE_OPEN_FROM_MPQ, byref(f)):
            self._fail(f"SFileOpenFileEx({archived})")
        try:
            high = DWORD()
            size = self.dll.SFileGetFileSize(f, byref(high))
            buf = ctypes.create_string_buffer(size)
            got = DWORD()
            if size and not self.dll.SFileReadFile(f, buf, size, byref(got), None):
                self._fail(f"SFileReadFile({archived})")
            return buf.raw[: got.value if size else 0]
        finally:
            self.dll.SFileCloseFile(f)

    def remove(self, archived: str) -> None:
        if self.has(archived) and not self.dll.SFileRemoveFile(self.handle, _name(archived), 0):
            self._fail(f"SFileRemoveFile({archived})")

    def write(self, archived: str, data: bytes) -> None:
        """Replaces (remove, then add) a file. smpq-style "add" silently keeps
        an existing entry, so the old one is always removed first."""
        self.remove(archived)
        if self.dll.SFileGetMaxFileCount(self.handle) <= len(self.names()) + 1:
            self.dll.SFileSetMaxFileCount(self.handle, self.dll.SFileGetMaxFileCount(self.handle) * 2)
        f = HANDLE()
        if not self.dll.SFileCreateFile(self.handle, _name(archived), 0, len(data), LANG_NEUTRAL,
                                        MPQ_FILE_COMPRESS | MPQ_FILE_REPLACEEXISTING, byref(f)):
            self._fail(f"SFileCreateFile({archived})")
        buf = ctypes.create_string_buffer(data, len(data))
        if data and not self.dll.SFileWriteFile(f, buf, len(data), MPQ_COMPRESSION_ZLIB):
            self._fail(f"SFileWriteFile({archived})")
        if not self.dll.SFileFinishFile(f):
            self._fail(f"SFileFinishFile({archived})")

    def compact(self) -> None:
        if not self.dll.SFileCompactArchive(self.handle, None, False):
            self._fail("SFileCompactArchive")

    def close(self) -> None:
        if self.handle:
            if self.writable:
                self.dll.SFileFlushArchive(self.handle)
            self.dll.SFileCloseArchive(self.handle)
            self.handle = HANDLE()

    def __enter__(self) -> "MPQ":
        return self

    def __exit__(self, *exc) -> None:
        self.close()
