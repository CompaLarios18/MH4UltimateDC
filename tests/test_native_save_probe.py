"""tools/native/save_probe.py (N0-31): synthetic data only (made-up bytes, a made-up key)."""
from __future__ import annotations

import struct
import sys
import zlib
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "native"))

import save_probe  # noqa: E402

KEY = b"synthetic test key, not the game's"

# Eric Young's Blowfish test vectors (big-endian words, the cipher's own published convention).
VECTORS = [
    ("0000000000000000", "0000000000000000", "4EF997456198DD78"),
    ("FFFFFFFFFFFFFFFF", "FFFFFFFFFFFFFFFF", "51866FD5B85ECB8A"),
    ("3000000000000000", "1000000000000001", "7D856F9A613063F2"),
    ("0123456789ABCDEF", "1111111111111111", "61F9C3802281B096"),
    ("FEDCBA9876543210", "0123456789ABCDEF", "0ACEAB0FC6A0A28D"),
]


def synthetic_slot(size: int = 4096) -> bytes:
    """A made-up slot body: a counter pattern with the header words left zero."""
    body = bytearray((i * 37 + 11) & 0xFF for i in range(size))
    struct.pack_into("<II", body, 0, 0, 0)
    return bytes(body)


def write(tmp_path: Path, name: str, data: bytes) -> str:
    p = tmp_path / name
    p.write_bytes(data)
    return str(p)


def run(capsys: pytest.CaptureFixture[str], *argv: str) -> tuple[int, str]:
    code = save_probe.main(list(argv))
    return code, capsys.readouterr().out


# ── Blowfish ──


@pytest.mark.parametrize("key,plain,cipher", VECTORS)
def test_blowfish_known_vectors(key: str, plain: str, cipher: str) -> None:
    bf = save_probe.Blowfish(bytes.fromhex(key))
    left, right = struct.unpack(">II", bytes.fromhex(plain))
    assert bf.encrypt_words(left, right) == struct.unpack(">II", bytes.fromhex(cipher))
    assert bf.decrypt_words(*struct.unpack(">II", bytes.fromhex(cipher))) == (left, right)


def test_pi_tables_start_with_the_standard_words() -> None:
    w = save_probe.pi_words()
    assert len(w) == 1042
    assert (w[0], w[1], w[18], w[1041]) == (0x243F6A88, 0x85A308D3, 0xD1310BA6, 0x3AC372E6)


def test_ecb_reads_words_little_endian() -> None:
    bf = save_probe.Blowfish(KEY)
    block = bytes(range(8))
    left, right = struct.unpack("<II", block)
    assert bf.encrypt_ecb(block) == struct.pack("<II", *bf.encrypt_words(left, right))
    assert bf.decrypt_ecb(bf.encrypt_ecb(block * 3)) == block * 3


def test_ecb_changes_whole_blocks_only() -> None:
    bf = save_probe.Blowfish(KEY)
    a = bytes(64)
    b = bytearray(a)
    b[17] ^= 1
    changed = [i for i in range(64) if bf.encrypt_ecb(a)[i] != bf.encrypt_ecb(bytes(b))[i]]
    assert changed and min(changed) >= 16 and max(changed) < 24


def test_ecb_needs_whole_blocks() -> None:
    with pytest.raises(save_probe.UsageError):
        save_probe.Blowfish(KEY).decrypt_ecb(bytes(12))


# ── the slot's layers ──


def test_xor_scramble_is_its_own_inverse_and_leaves_the_first_word() -> None:
    data = synthetic_slot(64)
    once = save_probe.xor_scramble(data, 0x1234)
    assert once != data and once[:4] == data[:4]
    assert save_probe.xor_scramble(once, 0x1234) == data


def test_xor_scramble_follows_the_recurrence_and_never_stalls_at_zero() -> None:
    out = save_probe.xor_scramble(bytes(12), 0)
    expected, x = [], 1
    for _ in range(4):
        x = (x * 0xB0) % 0xFF53
        expected.append(x)
    assert struct.unpack_from("<HHHH", out, 4) == tuple(expected)
    assert expected[0] == 0xB0 and expected[1] == 0xB0 * 0xB0


def test_seal_then_unwrap_gives_the_body_back_with_a_matching_sum() -> None:
    body = synthetic_slot()
    sealed = save_probe.seal(body, KEY, seed=0xA758)
    assert len(sealed) == len(body) and sealed != body
    plain, seed, stored, computed = save_probe.unwrap(sealed, KEY)
    assert seed == 0xA758 and stored == computed == save_probe.byte_sum(body)
    assert plain[8:] == body[8:] and struct.unpack_from("<I", plain, 0)[0] == 0x10


def test_unwrap_with_the_wrong_key_or_skipped_layer_does_not_match() -> None:
    sealed = save_probe.seal(synthetic_slot(), KEY, seed=77)
    _, _, stored, computed = save_probe.unwrap(sealed, b"another key")
    assert stored != computed
    _, _, stored, computed = save_probe.unwrap(sealed, KEY, xor=False)
    assert stored != computed


def test_a_changed_body_byte_changes_the_sum_and_exactly_its_blocks() -> None:
    body = bytearray(synthetic_slot())
    first = save_probe.seal(bytes(body), KEY, seed=5)
    body[300] ^= 0x40
    second = save_probe.seal(bytes(body), KEY, seed=5)
    ranges = save_probe.diff_ranges(first, second)
    starts = {r[0] // 8 for r in ranges}
    assert 0 in starts and 300 // 8 in starts and len(starts) == 2


# ── commands ──


def test_diff_prints_ranges_and_never_values(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    a = bytearray(b"\xc3" * 100)
    b = bytearray(a)
    b[0:8] = b"\x5a" * 8
    b[40:56] = b"\x5a" * 16
    code, out = run(capsys, "diff", write(tmp_path, "a", bytes(a)), write(tmp_path, "b", bytes(b)))
    assert code == 1
    assert "differs offset 0 count 8" in out and "differs offset 40 count 16" in out
    assert "2 ranges, 24 bytes differ" in out
    assert "c3" not in out.lower() and "5a" not in out.lower()


def test_diff_equal_files_exit_0_and_align_names_the_blocks(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    a = bytes(64)
    b = bytearray(a)
    b[17:20] = b"\x01\x01\x01"
    code, out = run(capsys, "diff", write(tmp_path, "a", a), write(tmp_path, "a2", a))
    assert code == 0 and "0 ranges" in out
    code, out = run(capsys, "diff", write(tmp_path, "a3", a), write(tmp_path, "b", bytes(b)), "--align", "8")
    assert code == 1 and "offset 17 count 3 blocks 2 to 2 of 8 bytes" in out


def test_diff_limit_ignores_the_tail_and_a_sized_range_at_the_edge(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    a = bytes(32) + b"\x01" * 8
    b = bytes(32) + b"\x02" * 8
    code, out = run(capsys, "diff", write(tmp_path, "a", a), write(tmp_path, "b", b), "--limit", "32")
    assert code == 0 and "compared 32 bytes" in out
    code, out = run(capsys, "diff", write(tmp_path, "a", a), write(tmp_path, "b", b))
    assert code == 1 and "offset 32 count 8" in out
    code, _ = run(capsys, "diff", write(tmp_path, "a", a), write(tmp_path, "b", b), "--limit", "99")
    assert code == 2


def test_ranges_zero_gives_the_crc_of_the_masked_file(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    a = bytearray(bytes(range(200)) * 3)
    b = bytearray(a)
    b[0:8] = b"\xee" * 8
    b[280:296] = b"\x11" * 16
    masks = ["--zero", "0:8", "280:16"]
    _, out_a = run(capsys, "ranges", write(tmp_path, "a", bytes(a)), *masks)
    _, out_b = run(capsys, "ranges", write(tmp_path, "b", bytes(b)), *masks)
    assert out_a == out_b and "zeroed 2 ranges" in out_a
    # the same CRC as zeroing by hand, and a one-byte difference outside the masks is seen
    expected = bytearray(a)
    expected[0:8] = bytes(8)
    expected[280:296] = bytes(16)
    assert f"0x{zlib.crc32(bytes(expected)):08x}" in out_a
    b[100] ^= 1
    _, out_c = run(capsys, "ranges", write(tmp_path, "c", bytes(b)), *masks)
    assert out_c != out_a
    assert run(capsys, "ranges", write(tmp_path, "c", bytes(b)), "--zero", "590:20")[0] == 2
    assert run(capsys, "ranges", write(tmp_path, "c", bytes(b)))[0] == 2


def test_ranges_prints_the_crc32_of_each_range(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    data = bytes(range(256)) * 4
    code, out = run(capsys, "ranges", write(tmp_path, "f", data), "--at", "0:8", "280:16", "0x10:0x20")
    assert code == 0
    for offset, size in ((0, 8), (280, 16), (16, 32)):
        assert f"offset {offset} size {size} crc32 0x{zlib.crc32(data[offset:offset + size]):08x}" in out
    code, _ = run(capsys, "ranges", write(tmp_path, "f", data), "--at", "1000:100")
    assert code == 2


@pytest.mark.parametrize("algo", save_probe.ALGOS)
def test_crc_check_matches_its_own_value_and_flags_another(
        tmp_path: Path, capsys: pytest.CaptureFixture[str], algo: str) -> None:
    body = bytearray(synthetic_slot(64))
    value = save_probe.checksum(algo, bytes(body[8:]))
    struct.pack_into("<I", body, 4, value)
    path = write(tmp_path, "s", bytes(body))
    code, out = run(capsys, "crc-check", path, "--algo", algo, "--from", "8", "--stored", "4:4")
    assert code == 0 and "; match" in out
    body[4] ^= 1
    code, out = run(capsys, "crc-check", write(tmp_path, "s2", bytes(body)), "--algo", algo, "--from", "8", "--stored", "4:4")
    assert code == 1 and "no match" in out


def test_checksum_definitions() -> None:
    assert save_probe.checksum("sum8", b"\xff\xff\x02") == 0x200
    assert save_probe.checksum("sum16", b"\x01\x00\x02\x00\x03") == 3
    assert save_probe.checksum("sum32", struct.pack("<II", 0xFFFFFFFF, 2)) == 1
    assert save_probe.checksum("xor32", struct.pack("<II", 0xF0F0, 0xFF)) == 0xF00F
    assert save_probe.checksum("crc32", b"123456789") == 0xCBF43926
    assert save_probe.checksum("adler32", b"Wikipedia") == 0x11E60398


def test_crc_check_rejects_bad_ranges(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    path = write(tmp_path, "s", bytes(16))
    assert run(capsys, "crc-check", path, "--algo", "crc32", "--from", "20")[0] == 2
    assert run(capsys, "crc-check", path, "--algo", "crc32", "--stored", "14:4")[0] == 2
    assert run(capsys, "crc-check", path, "--algo", "crc32", "--stored", "0:3")[0] == 2


def test_unwrap_command_with_each_key_source(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    body = synthetic_slot(1024)
    sealed = write(tmp_path, "slot", save_probe.seal(body, KEY, seed=0x4242))
    code_bin = bytearray(0x300)
    code_bin[0x120:0x120 + len(KEY) + 1] = KEY + b"\0"
    code_path = write(tmp_path, "code.bin", bytes(code_bin))
    key_path = write(tmp_path, "key.txt", KEY + b"\n")
    out_path = str(tmp_path / "plain.bin")
    for key_args in (["--key-from-code", f"{code_path}:0x{0x100000 + 0x120:x}"],
                     ["--key-hex", KEY.hex()], ["--key-file", key_path]):
        code, out = run(capsys, "unwrap", sealed, *key_args, "--out", out_path)
        assert code == 0 and "matches" in out and "1024 bytes, 128 blocks" in out
        assert Path(out_path).read_bytes()[8:] == body[8:]
        assert KEY.decode() not in out
    code, out = run(capsys, "unwrap", sealed, "--key-hex", b"other".hex())
    assert code == 1 and "does not match" in out


def test_unwrap_usage_errors(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    sealed = write(tmp_path, "slot", save_probe.seal(synthetic_slot(64), KEY, seed=1))
    assert run(capsys, "unwrap", sealed)[0] == 2
    assert run(capsys, "unwrap", sealed, "--key-hex", "zz")[0] == 2
    assert run(capsys, "unwrap", sealed, "--key-hex", "00", "--key-file", sealed)[0] == 2
    assert run(capsys, "unwrap", sealed, "--key-from-code", f"{sealed}:0x10")[0] == 2
    assert run(capsys, "unwrap", write(tmp_path, "odd", bytes(12)), "--key-hex", "00")[0] == 2
    assert run(capsys, "unwrap", str(tmp_path / "missing"), "--key-hex", "00")[0] == 2
