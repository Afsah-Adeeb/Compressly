"""
ctypes binding to the compressor's shared library.

Why ctypes and not a subprocess: spawning the CLI costs about 11 ms per call on this
machine, which on a 256 KB payload is more than the compression itself. A load test built
on that would be measuring process creation, and its p99 would say nothing about the
codec.

Why this matters for concurrency: ctypes releases the GIL for the duration of a foreign
call, so several Python request threads really do run C++ compression simultaneously. A
pure-Python implementation could not, and neither could an extension that held the GIL.
"""

from __future__ import annotations

import ctypes
import os
import sys
from pathlib import Path

_STATUS = {
    0: "ok",
    1: "invalid argument",
    2: "corrupt input",
    3: "out of memory",
    4: "internal error",
}


class CompressionError(RuntimeError):
    """Raised when the library reports a failure. `status` is the cmpr_status code."""

    def __init__(self, status: int, message: str):
        super().__init__(f"{_STATUS.get(status, 'error')}: {message}")
        self.status = status


def _default_library_path() -> Path:
    root = Path(__file__).resolve().parent.parent
    name = "cmpr.dll" if os.name == "nt" else "libcmpr.so"
    return root / "build" / name


class Compressor:
    """Thin wrapper over the C API. Safe to share across threads: the library keeps no
    mutable global state, and its error buffer is thread-local."""

    def __init__(self, library_path: str | os.PathLike | None = None):
        path = Path(library_path) if library_path else _default_library_path()
        if not path.exists():
            raise FileNotFoundError(
                f"shared library not found at {path}\n"
                f"build it first:  cmake --build build --target cmpr_c"
            )
        self._lib = ctypes.CDLL(str(path))
        self._bind()

    def _bind(self) -> None:
        lib = self._lib
        lib.cmpr_compress.restype = ctypes.c_int
        lib.cmpr_compress.argtypes = [
            ctypes.POINTER(ctypes.c_ubyte), ctypes.c_size_t, ctypes.c_int, ctypes.c_size_t,
            ctypes.POINTER(ctypes.POINTER(ctypes.c_ubyte)), ctypes.POINTER(ctypes.c_size_t),
        ]
        lib.cmpr_decompress.restype = ctypes.c_int
        lib.cmpr_decompress.argtypes = [
            ctypes.POINTER(ctypes.c_ubyte), ctypes.c_size_t,
            ctypes.POINTER(ctypes.POINTER(ctypes.c_ubyte)), ctypes.POINTER(ctypes.c_size_t),
        ]
        lib.cmpr_free.restype = None
        lib.cmpr_free.argtypes = [ctypes.POINTER(ctypes.c_ubyte)]
        lib.cmpr_last_error.restype = ctypes.c_char_p
        lib.cmpr_version.restype = ctypes.c_char_p

    def _call(self, function, *args) -> bytes:
        out = ctypes.POINTER(ctypes.c_ubyte)()
        out_size = ctypes.c_size_t()
        status = function(*args, ctypes.byref(out), ctypes.byref(out_size))
        if status != 0:
            raise CompressionError(status, self._lib.cmpr_last_error().decode("utf-8", "replace"))
        try:
            # The library owns this buffer until cmpr_free; the slice copies it into a
            # Python bytes object before it is released, in the `finally` below.
            return bytes(bytearray(out[: out_size.value])) if out_size.value else b""
        finally:
            self._lib.cmpr_free(out)

    @staticmethod
    def _as_buffer(data: bytes):
        if not data:
            return ctypes.POINTER(ctypes.c_ubyte)()
        return (ctypes.c_ubyte * len(data)).from_buffer_copy(data)

    def compress(self, data: bytes, threads: int = 1, block_size: int = 0) -> bytes:
        return self._call(self._lib.cmpr_compress, self._as_buffer(data), len(data),
                          threads, block_size)

    def decompress(self, data: bytes) -> bytes:
        return self._call(self._lib.cmpr_decompress, self._as_buffer(data), len(data))

    def version(self) -> str:
        return self._lib.cmpr_version().decode("utf-8")


if __name__ == "__main__":
    # Smoke test: python service/cmpr.py
    codec = Compressor()
    print(codec.version())
    sample = (b"the quick brown fox jumps over the lazy dog. " * 5000)
    packed = codec.compress(sample, threads=4)
    assert codec.decompress(packed) == sample, "roundtrip failed"
    print(f"{len(sample):,} -> {len(packed):,} bytes "
          f"({100 * (1 - len(packed) / len(sample)):.2f}% reduction), roundtrip ok")
    try:
        codec.decompress(b"not a compressed file at all")
    except CompressionError as error:
        print(f"corrupt input rejected: {error}")
        sys.exit(0)
    sys.exit("expected corrupt input to be rejected")
