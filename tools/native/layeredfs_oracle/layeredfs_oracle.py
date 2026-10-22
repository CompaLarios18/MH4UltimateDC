#!/usr/bin/env python3
"""The oracle of the native LayeredFS and patch code (N3-10): what Azahar's own code does.

    layeredfs_oracle.py --azahar ~/azahar [--keep DIR] layered BASE MODS_DIR OUT
    layeredfs_oracle.py --azahar ~/azahar [--keep DIR] patch [--kind ips|bps] BUFFER PATCH OUT

copies Azahar's layered_fs.{cpp,h}, patch.{cpp,h} and the small headers they need into a temporary
folder, applies the hunk of port/azahar/azahar.patch for layered_fs.cpp (the files in the sub-folders of
romfs_ext/ too; `git apply --include` picks the block by the file's name, not by line numbers, so a
regenerated patch with other hunks around it does not matter), compiles them with clang and the
stand-ins of standin/azahar_standin.h (FileUtil over the host, logging as nothing) and runs them.

  layered BASE MODS_DIR OUT   BASE is a level-3 RomFS image, MODS_DIR holds romfs/ and romfs_ext/; OUT gets
                              the whole layered image as LayeredFS::ReadFile gives it (what the RomFS
                              looks like to the game). Exit 3 when Azahar's code throws (it does for a
                              .stub on the first file of a folder).
  patch BUFFER PATCH OUT      applies one IPS or BPS patch to the bytes of BUFFER with Azahar's
                              Patch::ApplyIpsPatch / ApplyBpsPatch and writes the result; the kind is the
                              patch file's extension (.ips is IPS, anything else BPS, as LayeredFS reads
                              it) or --kind. Exit 2 when Azahar's code says the patch is an error. N3-09
                              uses it for code patches.

--keep DIR builds in DIR and keeps the driver there: a later call with the same sources reuses it.

Exit status: the driver's (0 ok, 1 usage or unreadable input, 2 Azahar's patch error, 3 Azahar threw), or
4 when the harness itself fails (the sources are not at the pinned commit, the patch does not apply, the
compiler fails).
"""
from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
PATCH = os.path.join(ROOT, "port", "azahar", "azahar.patch")
PINNED = os.path.join(ROOT, "port", "azahar", "COMMIT")
LAYERED = "src/core/file_sys/layered_fs.cpp"

# Azahar files copied as they are (relative to src/)
COPIED = (
    "common/common_types.h",
    "common/swap.h",
    "common/alignment.h",
    "core/file_sys/layered_fs.cpp",
    "core/file_sys/layered_fs.h",
    "core/file_sys/patch.cpp",
    "core/file_sys/patch.h",
)
# the Azahar headers that the stand-in header replaces: each is a forwarding header
FORWARDED = (
    "common/archives.h",
    "common/assert.h",
    "common/common_paths.h",
    "common/file_util.h",
    "common/string_util.h",
    "common/logging/log.h",
    "core/file_sys/romfs_reader.h",
    "core/loader/loader.h",
    "boost/crc.hpp",
    "boost/serialization/base_object.hpp",
    "boost/serialization/export.hpp",
    "boost/serialization/shared_ptr.hpp",
    "boost/serialization/string.hpp",
)


class OracleError(RuntimeError):
    pass


def pinned_commit() -> str:
    with open(PINNED, encoding="utf-8") as f:
        return f.read().strip()


def azahar_commit(azahar: str) -> str:
    # the ceiling keeps git from finding a checkout around a folder that is not one
    env = dict(os.environ, GIT_CEILING_DIRECTORIES=os.path.dirname(os.path.abspath(azahar)))
    done = subprocess.run(["git", "-C", azahar, "rev-parse", "HEAD"], capture_output=True, text=True, env=env)
    if done.returncode != 0:
        raise OracleError(f"{azahar} is not a git checkout of Azahar")
    return done.stdout.strip()


def _hash_sources(commit: str) -> str:
    h = hashlib.sha256(commit.encode())
    for path in (os.path.join(HERE, "driver.cpp"), os.path.join(HERE, "standin", "azahar_standin.h"), PATCH):
        with open(path, "rb") as f:
            h.update(hashlib.sha256(f.read()).digest())
    return h.hexdigest()


def prepare_tree(azahar: str, tree: str) -> None:
    """Copy Azahar's files into tree/src, apply the port's hunk for layered_fs.cpp, write the forwarding headers."""
    for rel in COPIED:
        dst = os.path.join(tree, "src", rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(os.path.join(azahar, "src", rel), dst)
    # The hunk is found by the file's name (git apply --include), whatever else the patch changes and
    # wherever the block sits. The ceiling keeps git to the temporary folder, which may be inside a checkout.
    env = dict(os.environ, GIT_CEILING_DIRECTORIES=os.path.dirname(os.path.abspath(tree)))
    done = subprocess.run(["git", "apply", f"--include={LAYERED}", "-p1", PATCH], cwd=tree,
                          capture_output=True, text=True, env=env)
    if done.returncode != 0:
        raise OracleError("the port's hunk for layered_fs.cpp does not apply to Azahar's file:\n" + done.stderr[-2000:])
    with open(os.path.join(tree, "src", LAYERED[4:]), encoding="utf-8") as f:
        if "GetAllFilesFromNestedEntries" not in f.read():
            raise OracleError("the patch has no hunk for layered_fs.cpp (the files in the sub-folders of romfs_ext/)")
    for rel in FORWARDED:
        dst = os.path.join(tree, "src", rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "w", encoding="utf-8") as f:
            f.write('#pragma once\n#include "azahar_standin.h"\n')
    shutil.copyfile(os.path.join(HERE, "standin", "azahar_standin.h"), os.path.join(tree, "src", "azahar_standin.h"))


def compile_driver(tree: str, cxx: str) -> str:
    exe = os.path.join(tree, "layeredfs_driver")
    src = os.path.join(tree, "src")
    cmd = [cxx, "-std=c++20", "-O1", "-w", "-I", src,
           os.path.join(src, "core", "file_sys", "layered_fs.cpp"),
           os.path.join(src, "core", "file_sys", "patch.cpp"),
           os.path.join(HERE, "driver.cpp"), "-o", exe]
    done = subprocess.run(cmd, capture_output=True, text=True)
    if done.returncode != 0:
        raise OracleError("compiling Azahar's LayeredFS code failed:\n" + done.stderr[-4000:])
    return exe


def build_driver(azahar: str, cxx: str = "clang++", keep: str | None = None) -> str:
    """Builds the driver and returns its path. With `keep`, the build is kept in that folder and reused while
    the sources are the same. Without it, the driver is in a new temporary folder the caller may delete."""
    azahar = os.path.abspath(os.path.expanduser(azahar))
    commit = azahar_commit(azahar)
    if commit != pinned_commit():
        raise OracleError(f"{azahar} is at {commit}, the pinned commit is {pinned_commit()}")
    if shutil.which(cxx) is None:
        raise OracleError(f"{cxx} not found")
    stamp = _hash_sources(commit)
    tree = os.path.abspath(keep) if keep else tempfile.mkdtemp(prefix="layeredfs_oracle_")
    exe = os.path.join(tree, "layeredfs_driver")
    stamp_path = os.path.join(tree, "stamp")
    if keep and os.path.exists(exe) and os.path.exists(stamp_path):
        with open(stamp_path, encoding="utf-8") as f:
            if f.read() == stamp:
                return exe
    try:
        os.makedirs(tree, exist_ok=True)
        stale = os.path.join(tree, "src")
        if os.path.isdir(stale):
            shutil.rmtree(stale)
        prepare_tree(azahar, tree)
        exe = compile_driver(tree, cxx)
        with open(stamp_path, "w", encoding="utf-8") as f:
            f.write(stamp)
    except Exception:
        if not keep:
            shutil.rmtree(tree, ignore_errors=True)
        raise
    return exe


def patch_kind(patch_path: str, kind: str | None = None) -> str:
    return kind or ("ips" if patch_path.lower().endswith(".ips") else "bps")


def run_layered(exe: str, base: str, mods_dir: str, out: str) -> subprocess.CompletedProcess:
    return subprocess.run([exe, "layered", base, mods_dir, out], capture_output=True, text=True)


def run_patch(exe: str, buffer: str, patch: str, out: str, kind: str | None = None) -> subprocess.CompletedProcess:
    return subprocess.run([exe, "patch", patch_kind(patch, kind), buffer, patch, out], capture_output=True, text=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--azahar", required=True, help="Azahar's sources at the pinned commit (~/azahar)")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++"))
    parser.add_argument("--keep", help="build in this folder and keep it")
    sub = parser.add_subparsers(dest="mode", required=True)
    layered = sub.add_parser("layered")
    layered.add_argument("base")
    layered.add_argument("mods_dir")
    layered.add_argument("out")
    patch = sub.add_parser("patch")
    patch.add_argument("--kind", choices=("ips", "bps"))
    patch.add_argument("buffer")
    patch.add_argument("patch")
    patch.add_argument("out")
    args = parser.parse_args(argv)
    try:
        exe = build_driver(args.azahar, args.cxx, args.keep)
    except (OracleError, OSError) as error:
        print(f"layeredfs_oracle: {error}", file=sys.stderr)
        return 4
    try:
        if args.mode == "layered":
            done = run_layered(exe, args.base, args.mods_dir, args.out)
        else:
            done = run_patch(exe, args.buffer, args.patch, args.out, args.kind)
    finally:
        if not args.keep:
            shutil.rmtree(os.path.dirname(exe), ignore_errors=True)
    sys.stderr.write(done.stderr)
    sys.stdout.write(done.stdout)
    return done.returncode


if __name__ == "__main__":
    sys.exit(main())
