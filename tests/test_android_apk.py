"""Checks on a built Android APK: the patched native core, and the player's own code library
when one is pointed at, are really in the package.

Skips without ``MH4U_APK``. The further checks need the paths they compare against, which
are the outputs of the same build:

* ``MH4U_CORE_SO``: the CMake intermediate ``libcitra-android.so``
  (``app/build/intermediates/cxx/Debug/<hash>/obj/arm64-v8a/libcitra-android.so``),
* ``MH4U_CODE_LIBS``: the folder passed to Gradle as ``-Pmh4uCodeLibs`` (an
  ``arm64-v8a/libmh4u_code.so`` built by the player from their own dump).

The package's libraries are stripped on the way in, so identity is the ELF build ID, which
survives stripping, not a file hash. Nothing from a game file is read here: the checks are
on the package structure, the build IDs and strings compiled into the libraries.
"""
from __future__ import annotations

import os
import struct
import zipfile

import pytest

APK = os.environ.get("MH4U_APK")
CORE_SO = os.environ.get("MH4U_CORE_SO")
CODE_LIBS = os.environ.get("MH4U_CODE_LIBS")

pytestmark = pytest.mark.skipif(not APK, reason="set MH4U_APK to a built app-vanilla-debug.apk")


def build_id(data: bytes) -> str | None:
    """The GNU build ID of an ELF image, or None if it has none."""
    try:
        from elftools.elf.elffile import ELFFile
    except ImportError:  # pyelftools is in requirements.txt; don't fail the suite without it
        pytest.skip("pyelftools not installed")
    import io

    elf = ELFFile(io.BytesIO(data))
    section = elf.get_section_by_name(".note.gnu.build-id")
    if section is None:
        return None
    note = section.data()
    namesz, descsz, _ = struct.unpack_from("<III", note, 0)
    offset = 12 + ((namesz + 3) & ~3)
    return note[offset:offset + descsz].hex()


def packaged(name: str) -> bytes:
    with zipfile.ZipFile(APK) as apk:
        assert name in apk.namelist(), name
        return apk.read(name)


def test_apk_has_the_patched_arm64_core():
    assert os.path.exists(APK)
    core = packaged("lib/arm64-v8a/libcitra-android.so")
    # log format strings from the fixes in this branch: their presence shows the packaged
    # binary comes from the patched sources
    assert b"User directory native path" in core       # native.cpp, user-directory cache
    assert b"FormatSaveData {:016X}" in core           # archive_source_sd_savedata.cpp


@pytest.mark.skipif(not CORE_SO, reason="set MH4U_CORE_SO to the built libcitra-android.so")
def test_apk_core_is_the_library_just_built():
    packaged_id = build_id(packaged("lib/arm64-v8a/libcitra-android.so"))
    with open(CORE_SO, "rb") as f:
        built_id = build_id(f.read())
    assert built_id is not None, "the built core has no build ID to compare"
    assert packaged_id == built_id, "the APK has a stale libcitra-android.so: package it again"


@pytest.mark.skipif(not CODE_LIBS, reason="set MH4U_CODE_LIBS to the -Pmh4uCodeLibs folder")
def test_apk_packages_the_given_code_library():
    lib = os.path.join(CODE_LIBS, "arm64-v8a", "libmh4u_code.so")
    assert os.path.exists(lib), lib
    packaged_id = build_id(packaged("lib/arm64-v8a/libmh4u_code.so"))
    with open(lib, "rb") as f:
        built = f.read()
    assert built[:4] == b"\x7fELF" and built[18:20] == b"\xb7\x00"  # arm64-v8a (EM_AARCH64)
    built_id = build_id(built)
    assert packaged_id is not None and packaged_id == built_id, "the APK has a stale code library"
