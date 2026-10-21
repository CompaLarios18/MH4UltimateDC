#!/usr/bin/env python3
"""Probe the bytes of a game save buffer without ever printing them (N0-31, docs/research/g2-save-bytes.md).

The commands print offsets, sizes, counts, CRCs and yes/no answers; never the bytes of a file.

  diff A B [--limit N] [--align K]
      The byte ranges where the first N bytes (default: all of the shorter file) of two files differ,
      one `offset count` per line. `--align K` adds the K-byte blocks a range touches (an ECB cipher
      with 8-byte blocks changes whole blocks: `--align 8`). Exit 0 when the files are equal over the
      compared bytes, 1 when they are not.

  ranges FILE [--at OFFSET:SIZE...] [--zero OFFSET:SIZE...]
      The CRC-32 of each --at range of FILE, as 0x%08x. With --zero, also the CRC-32 of the whole
      file after those ranges are set to zero (what a write mask of the comparators does: two
      buffers whose masked CRC-32 agree differ only inside the masked ranges).

  crc-check FILE --algo ALGO [--from OFFSET] [--to OFFSET] [--stored OFFSET:SIZE]
      A checksum candidate over FILE[from:to] (default: all of it): ALGO is crc32, adler32, sum8 (the
      sum of the bytes, modulo 2^32), sum16 or sum32 (the sum of the little-endian words, modulo
      2^32) or xor32 (the XOR of the little-endian words). With --stored, the value is compared to the
      little-endian integer stored there. Exit 0 when it matches (or when nothing is compared to it), 1
      when it does not.

  unwrap FILE (--key-from-code CODE:ADDR | --key-hex HEX | --key-file F) [--out OUT] [--no-xor]
      Undoes the three layers the game puts on a user save slot (functions 0x002ab95c, 0x002abcac and
      0x002ac1c0 of the 1.3.0 code; see the document): Blowfish in ECB mode on 8-byte blocks read as
      two little-endian words, then a halfword XOR keystream whose seed is the upper half of the first
      word, then a header whose second word is the sum of the bytes from offset 8 on. Prints the sizes
      and whether the stored sum matches; writes the plaintext to OUT when given. The key is the
      NUL-terminated string at address ADDR of the decompressed code file CODE (whose first byte is at
      0x00100000), or hex bytes, or a file. Exit 0 when the checksum matches, 1 when it does not.

Exit status: 0 ok, 1 a difference or a mismatch, 2 a usage problem or an unreadable input.
Standard library only. The Blowfish tables are computed from the hexadecimal digits of pi (the
tables the game carries are the standard ones: the first words of the code's tables are checked in
the document), so no game data is needed to run the tests.
"""
from __future__ import annotations

import argparse
import functools
import struct
import sys
import zlib
from pathlib import Path
from typing import Iterable

CODE_BASE = 0x00100000      # the address of the first byte of the decompressed code file
HEADER = 8                  # the user slot's header: two little-endian words
XOR_MODULUS = 0xFF53        # the keystream's modulus (0x0000ff53 in the game's literal pool)
XOR_MULTIPLIER = 0xB0       # and its multiplier
KEY_MAX = 4096


class UsageError(Exception):
    """A command line or an input that cannot be used."""


# ── Blowfish (ECB, words little-endian), as the game uses it ───────────────────────────


@functools.lru_cache(maxsize=1)
def pi_words() -> tuple[int, ...]:
    """The first 1042 32-bit words of the fractional part of pi: P[18] then S0..S3[256 each]."""
    count = 18 + 4 * 256
    bits = count * 32 + 64
    one = 1 << bits

    def arctan_inv(x: int) -> int:
        total = term = one // x
        x2, n, sign = x * x, 3, -1
        while term:
            term //= x2
            total += sign * (term // n)
            sign, n = -sign, n + 2
        return total

    pi = 4 * (4 * arctan_inv(5) - arctan_inv(239))
    frac = (pi - 3 * one) >> 64
    return tuple((frac >> ((count - 1 - i) * 32)) & 0xFFFFFFFF for i in range(count))


class Blowfish:
    def __init__(self, key: bytes) -> None:
        if not key:
            raise UsageError("empty Blowfish key")
        words = pi_words()
        self.p = list(words[:18])
        self.s = [list(words[18 + 256 * i: 18 + 256 * (i + 1)]) for i in range(4)]
        for i in range(18):
            w = 0
            for j in range(4):
                w = (w << 8) | key[(i * 4 + j) % len(key)]
            self.p[i] ^= w
        left = right = 0
        for i in range(0, 18, 2):
            left, right = self.encrypt_words(left, right)
            self.p[i], self.p[i + 1] = left, right
        for box in self.s:
            for i in range(0, 256, 2):
                left, right = self.encrypt_words(left, right)
                box[i], box[i + 1] = left, right

    def _f(self, x: int) -> int:
        s = self.s
        return ((((s[0][x >> 24] + s[1][(x >> 16) & 255]) & 0xFFFFFFFF) ^ s[2][(x >> 8) & 255])
                + s[3][x & 255]) & 0xFFFFFFFF

    def encrypt_words(self, left: int, right: int) -> tuple[int, int]:
        p = self.p
        for i in range(16):
            left ^= p[i]
            right ^= self._f(left)
            left, right = right, left
        left, right = right, left
        return left ^ p[17], right ^ p[16]

    def decrypt_words(self, left: int, right: int) -> tuple[int, int]:
        p = self.p
        for i in range(17, 1, -1):
            left ^= p[i]
            right ^= self._f(left)
            left, right = right, left
        left, right = right, left
        return left ^ p[0], right ^ p[1]

    def _ecb(self, data: bytes, op) -> bytes:
        if len(data) % 8:
            raise UsageError(f"size {len(data)} is not a multiple of 8")
        out = bytearray(len(data))
        for off in range(0, len(data), 8):
            left, right = struct.unpack_from("<II", data, off)
            struct.pack_into("<II", out, off, *op(left, right))
        return bytes(out)

    def decrypt_ecb(self, data: bytes) -> bytes:
        return self._ecb(data, self.decrypt_words)

    def encrypt_ecb(self, data: bytes) -> bytes:
        return self._ecb(data, self.encrypt_words)


# ── the slot's other two layers ────────────────────────────────────────────────────────


def xor_scramble(data: bytes, seed: int) -> bytes:
    """The halfword XOR layer (0x002ac1c0): from offset 4, each halfword is XORed with the next value
    of x -> (x or 1) * 0xb0 mod 0xff53, started from `seed`. Its own inverse."""
    out = bytearray(data)
    x = seed
    for off in range(4, len(out) - 1, 2):
        if x == 0:
            x = 1
        x = (x * XOR_MULTIPLIER) % XOR_MODULUS
        struct.pack_into("<H", out, off, struct.unpack_from("<H", out, off)[0] ^ x)
    return bytes(out)


def byte_sum(data: bytes, start: int = HEADER) -> int:
    return sum(data[start:]) & 0xFFFFFFFF


def seal(plain: bytes, key: bytes, seed: int, version: int = 0x10) -> bytes:
    """What the game does to a slot before writing it (0x002abe04): the header's first word becomes
    version | seed << 16, the second the byte sum, then the XOR layer, then Blowfish. For tests."""
    body = bytearray(plain)
    struct.pack_into("<II", body, 0, (version & 0xFFFF) | ((seed & 0xFFFF) << 16), byte_sum(bytes(body)))
    return Blowfish(key).encrypt_ecb(xor_scramble(bytes(body), seed))


def unwrap(data: bytes, key: bytes, xor: bool = True) -> tuple[bytes, int, int, int]:
    """(plaintext, seed, stored sum, computed sum). The plaintext's first word keeps only the low half."""
    clear = Blowfish(key).decrypt_ecb(data)
    if len(clear) < HEADER:
        raise UsageError("a slot has at least a header")
    word0 = struct.unpack_from("<I", clear, 0)[0]
    seed = word0 >> 16
    if xor:
        clear = xor_scramble(clear, seed)
    plain = bytearray(clear)
    struct.pack_into("<I", plain, 0, word0 & 0xFFFF)
    return bytes(plain), seed, struct.unpack_from("<I", plain, 4)[0], byte_sum(bytes(plain))


# ── commands ───────────────────────────────────────────────────────────────────────────


def read_file(path: str) -> bytes:
    try:
        return Path(path).read_bytes()
    except OSError as e:
        raise UsageError(f"cannot read {path}: {e.strerror}") from None


def number(text: str, what: str) -> int:
    try:
        return int(text, 0)
    except ValueError:
        raise UsageError(f"{what}: {text!r} is not a number") from None


def span(text: str, what: str) -> tuple[int, int]:
    offset, sep, size = text.partition(":")
    if not sep:
        raise UsageError(f"{what}: {text!r} is not OFFSET:SIZE")
    return number(offset, what), number(size, what)


def diff_ranges(a: bytes, b: bytes, limit: int | None = None) -> list[tuple[int, int]]:
    n = min(len(a), len(b)) if limit is None else limit
    if n > min(len(a), len(b)):
        raise UsageError(f"--limit {n} is beyond the shorter file ({min(len(a), len(b))} bytes)")
    out: list[tuple[int, int]] = []
    start = None
    for i in range(n):
        if a[i] != b[i]:
            if start is None:
                start = i
        elif start is not None:
            out.append((start, i - start))
            start = None
    if start is not None:
        out.append((start, n - start))
    return out


def cmd_diff(args: argparse.Namespace) -> int:
    a, b = read_file(args.a), read_file(args.b)
    ranges = diff_ranges(a, b, args.limit)
    compared = min(len(a), len(b)) if args.limit is None else args.limit
    print(f"compared {compared} bytes: {len(a)} and {len(b)} in the files")
    for offset, count in ranges:
        line = f"differs offset {offset} count {count}"
        if args.align:
            k = args.align
            line += f" blocks {offset // k} to {(offset + count - 1) // k} of {k} bytes"
        print(line)
    print(f"{len(ranges)} ranges, {sum(c for _, c in ranges)} bytes differ")
    return 1 if ranges else 0


def checked_span(text: str, what: str, length: int) -> tuple[int, int]:
    offset, size = span(text, what)
    if offset < 0 or size < 0 or offset + size > length:
        raise UsageError(f"{what} {text} is outside the file ({length} bytes)")
    return offset, size


def cmd_ranges(args: argparse.Namespace) -> int:
    data = read_file(args.file)
    if not args.at and not args.zero:
        raise UsageError("give --at or --zero")
    for text in args.at or []:
        offset, size = checked_span(text, "--at", len(data))
        print(f"offset {offset} size {size} crc32 0x{zlib.crc32(data[offset:offset + size]) & 0xFFFFFFFF:08x}")
    if args.zero:
        masked = bytearray(data)
        for text in args.zero:
            offset, size = checked_span(text, "--zero", len(data))
            masked[offset:offset + size] = bytes(size)
        print(f"zeroed {len(args.zero)} ranges, whole file crc32 0x{zlib.crc32(bytes(masked)) & 0xFFFFFFFF:08x}")
    return 0


def words(data: bytes) -> Iterable[int]:
    tail = len(data) % 4
    padded = data + b"\0" * ((4 - tail) % 4)
    return (struct.unpack_from("<I", padded, i)[0] for i in range(0, len(padded), 4))


def checksum(algo: str, data: bytes) -> int:
    if algo == "crc32":
        return zlib.crc32(data) & 0xFFFFFFFF
    if algo == "adler32":
        return zlib.adler32(data) & 0xFFFFFFFF
    if algo == "sum8":
        return sum(data) & 0xFFFFFFFF
    if algo == "sum16":
        return sum(struct.unpack_from("<H", data, i)[0] for i in range(0, len(data) - 1, 2)) & 0xFFFFFFFF
    if algo == "sum32":
        return sum(words(data)) & 0xFFFFFFFF
    if algo == "xor32":
        x = 0
        for w in words(data):
            x ^= w
        return x
    raise UsageError(f"unknown algorithm {algo!r}")


ALGOS = ("crc32", "adler32", "sum8", "sum16", "sum32", "xor32")


def cmd_crc_check(args: argparse.Namespace) -> int:
    data = read_file(args.file)
    start = args.start
    end = len(data) if args.end is None else args.end
    if not 0 <= start <= end <= len(data):
        raise UsageError(f"range {start}..{end} is outside the file ({len(data)} bytes)")
    value = checksum(args.algo, data[start:end])
    line = f"{args.algo} over {start}..{end} = 0x{value:08x}"
    if args.stored is None:
        print(line)
        return 0
    offset, size = span(args.stored, "--stored")
    if size not in (1, 2, 4) or offset < 0 or offset + size > len(data):
        raise UsageError(f"--stored {args.stored} is not a 1, 2 or 4-byte field inside the file")
    stored = int.from_bytes(data[offset:offset + size], "little")
    ok = stored == value & ((1 << (8 * size)) - 1)
    print(f"{line}; stored at {offset}:{size} = 0x{stored:0{2 * size}x}; {'match' if ok else 'no match'}")
    return 0 if ok else 1


def load_key(args: argparse.Namespace) -> bytes:
    given = [x for x in (args.key_from_code, args.key_hex, args.key_file) if x]
    if len(given) != 1:
        raise UsageError("give exactly one of --key-from-code, --key-hex, --key-file")
    if args.key_hex:
        try:
            return bytes.fromhex(args.key_hex)
        except ValueError:
            raise UsageError("--key-hex is not hex") from None
    if args.key_file:
        return read_file(args.key_file).rstrip(b"\r\n")
    path, sep, addr = args.key_from_code.rpartition(":")
    if not sep or not path:
        raise UsageError("--key-from-code is CODE:ADDR")
    code = read_file(path)
    offset = number(addr, "--key-from-code") - CODE_BASE
    if not 0 <= offset < len(code):
        raise UsageError(f"address {addr} is outside the code file")
    end = code.find(b"\0", offset, offset + KEY_MAX)
    if end <= offset:
        raise UsageError(f"no NUL-terminated string at {addr}")
    return code[offset:end]


def cmd_unwrap(args: argparse.Namespace) -> int:
    data = read_file(args.file)
    key = load_key(args)
    plain, seed, stored, computed = unwrap(data, key, xor=not args.no_xor)
    ok = stored == computed
    print(f"{len(data)} bytes, {len(data) // 8} blocks of 8; key {len(key)} bytes; "
          f"stored sum {'matches' if ok else 'does not match'} the sum of bytes 8..{len(data)} "
          f"(stored 0x{stored:08x}, computed 0x{computed:08x}); keystream seed "
          f"{'zero' if seed == 0 else 'non-zero'}")
    if args.out:
        try:
            Path(args.out).write_bytes(plain)
        except OSError as e:
            raise UsageError(f"cannot write {args.out}: {e.strerror}") from None
        print(f"plaintext written to {args.out}")
    return 0 if ok else 1


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="command", required=True)

    d = sub.add_parser("diff", help="byte ranges that differ between two files")
    d.add_argument("a")
    d.add_argument("b")
    d.add_argument("--limit", type=lambda s: number(s, "--limit"))
    d.add_argument("--align", type=lambda s: number(s, "--align"), default=0)
    d.set_defaults(func=cmd_diff)

    r = sub.add_parser("ranges", help="CRC-32 of byte ranges")
    r.add_argument("file")
    r.add_argument("--at", nargs="+", metavar="OFFSET:SIZE")
    r.add_argument("--zero", nargs="+", metavar="OFFSET:SIZE")
    r.set_defaults(func=cmd_ranges)

    c = sub.add_parser("crc-check", help="a checksum candidate over a range, against a stored value")
    c.add_argument("file")
    c.add_argument("--algo", required=True, choices=ALGOS)
    c.add_argument("--from", dest="start", type=lambda s: number(s, "--from"), default=0)
    c.add_argument("--to", dest="end", type=lambda s: number(s, "--to"), default=None)
    c.add_argument("--stored", metavar="OFFSET:SIZE")
    c.set_defaults(func=cmd_crc_check)

    u = sub.add_parser("unwrap", help="undo the game's three layers on a user slot")
    u.add_argument("file")
    u.add_argument("--key-from-code", metavar="CODE:ADDR")
    u.add_argument("--key-hex")
    u.add_argument("--key-file")
    u.add_argument("--out")
    u.add_argument("--no-xor", action="store_true", help="skip the XOR layer (only Blowfish)")
    u.set_defaults(func=cmd_unwrap)
    return ap


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except UsageError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
