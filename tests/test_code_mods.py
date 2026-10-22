"""tools/code_mods.py: IPS/BPS code patches and Azahar's mod folder, on synthetic data."""
import os
import sys
import zlib

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import code_mods  # noqa: E402


def ips(*records, tail=b""):
    """records: (offset, data) or (offset, count, byte) for a run."""
    out = bytearray(b"PATCH")
    for r in records:
        out += r[0].to_bytes(3, "big")
        if len(r) == 2:
            out += len(r[1]).to_bytes(2, "big") + r[1]
        else:
            out += b"\0\0" + r[1].to_bytes(2, "big") + bytes([r[2]])
    return bytes(out + b"EOF" + tail)


def number(n):
    """BPS variable-length number."""
    out = bytearray()
    while True:
        x = n & 0x7F
        n >>= 7
        if n == 0:
            out.append(0x80 | x)
            return bytes(out)
        out.append(x)
        n -= 1


def bps(source, target, commands, source_crc=None, metadata=b""):
    """commands: ("sr", n) | ("tr", bytes) | ("sc", n, delta) | ("tc", n, delta)."""
    body = bytearray(b"BPS1" + number(len(source)) + number(len(target)) + number(len(metadata)) + metadata)
    for c in commands:
        if c[0] == "sr":
            body += number((c[1] - 1) << 2)
        elif c[0] == "tr":
            body += number(((len(c[1]) - 1) << 2) | 1) + c[1]
        else:
            body += number(((c[1] - 1) << 2) | (2 if c[0] == "sc" else 3))
            body += number(abs(c[2]) << 1 | (1 if c[2] < 0 else 0))
    body += (zlib.crc32(source) if source_crc is None else source_crc).to_bytes(4, "little")
    body += zlib.crc32(target).to_bytes(4, "little")
    return bytes(body + zlib.crc32(bytes(body)).to_bytes(4, "little"))


def test_ips_records_runs_and_eof():
    code = bytes(range(16))
    out = code_mods.apply_ips(code, ips((2, b"\xAA\xBB"), (10, 3, 0x55), tail=b"\0\0\x10"))
    assert out == bytes([0, 1, 0xAA, 0xBB, 4, 5, 6, 7, 8, 9, 0x55, 0x55, 0x55, 13, 14, 15])
    with pytest.raises(code_mods.ModError):
        code_mods.apply_ips(code, ips((15, b"\1\2")))       # past the end
    with pytest.raises(code_mods.ModError):
        code_mods.apply_ips(code, b"NOTIPS")


def test_bps_commands_and_checks():
    source = b"Monster Hunter 4"
    target = b"Monster Hunter Ultimate!!!!"
    # "Monster Hunter " from the source, "Ultimate!" from the patch, then "!" three times from the
    # target itself (a copy that reads what it has just written)
    patch = bps(source, target, [("sr", 15), ("tr", b"Ultimate!"), ("tc", 3, 23)])
    assert code_mods.apply_bps(source, patch) == target
    # a source copy from elsewhere in the source, and a shorter target keeps the code's length
    patch = bps(source, b"Hunter", [("sc", 6, 8)])
    assert code_mods.apply_bps(source, patch) == b"Hunter" + bytes(len(source) - 6)
    with pytest.raises(code_mods.ModError):     # made for another code.bin
        code_mods.apply_bps(source, bps(source, target, [("sr", 15), ("tr", b"Ultimate!"), ("tc", 3, 23)],
                                        source_crc=1))
    with pytest.raises(code_mods.ModError):     # Azahar refuses metadata
        code_mods.apply_bps(source, bps(source, source, [("sr", 16)], metadata=b"x"))


def test_mod_folder_like_azahar(tmp_path):
    base = tmp_path / "load" / "mods" / "0004000000126300"
    assert code_mods.mod_dir(str(tmp_path), 0x0004000E00126300) == str(base)   # an update's mods
    assert code_mods.find_mods(str(tmp_path), 0x0004000000126300) == (None, None)
    (base / "exefs").mkdir(parents=True)
    (base / "code.bps").write_bytes(b"x")
    (base / "exefs" / "code.ips").write_bytes(ips((1, b"\x09")))
    replacement, patch = code_mods.find_mods(str(tmp_path), 0x0004000000126300)
    assert replacement is None and patch == str(base / "exefs" / "code.ips")   # exefs/code.ips first
    (base / "code.bin").write_bytes(bytes(4))
    code, used = code_mods.apply_mods(b"\1\2\3\4", str(tmp_path), 0x0004000000126300)
    assert code == b"\0\x09\0\0" and used == [str(base / "code.bin"), str(base / "exefs" / "code.ips")]
    assert code_mods.describe(b"\1\2\3\4", code, [("code", 0, 2), ("data", 2, 2)]) == \
        ["code: 2 bytes changed", "data: 2 bytes changed"]


PID = 0x0004000000126300


def _mod_files(tmp_path, files):
    base = tmp_path / "load" / "mods" / f"{PID:016X}"
    for rel, data in files.items():
        (base / rel).parent.mkdir(parents=True, exist_ok=True)
        (base / rel).write_bytes(data)
    return str(tmp_path)


def test_bss_pages_size_rounds_up_to_pages():
    assert [code_mods.bss_pages_size(n) for n in (0, 1, 0x1000, 0x1001)] == [0, 0x1000, 0x1000, 0x2000]


def test_ips_record_and_run_into_the_bss_pages_are_dropped(tmp_path):
    code = bytes(range(16))
    user = _mod_files(tmp_path, {"exefs/code.ips": ips((2, b"\xAA\xBB"), (14, b"\x01\x02\x03\x04"),
                                                         (0x10 + 0x20, 8, 0x77))})
    with pytest.raises(code_mods.ModError, match="goes past the end"):     # no pages: as before
        code_mods.apply_mods(code, user, PID)
    out, used = code_mods.apply_mods(code, user, PID, bss_size=0x10)       # one page of bss
    assert out == code[:2] + b"\xAA\xBB" + code[4:14] + b"\x01\x02" and len(used) == 1
    with pytest.raises(code_mods.ModError, match="goes past the end"):     # the pages end at 0x1010
        code_mods.apply_mods(code, _mod_files(tmp_path / "b", {"code.ips": ips((0x1008, b"\x01" * 16))}), PID, 0x10)


def test_bps_made_for_the_code_and_its_pages(tmp_path):
    code = bytes(range(16))
    source = code + bytes(0x1000)
    target = bytearray(source)
    target[4:6] = b"ok"
    target[0x20:0x24] = b"bss!"
    user = _mod_files(tmp_path, {"code.bps": bps(source, bytes(target), _bps_commands(source, bytes(target)))})
    out, _ = code_mods.apply_mods(code, user, PID, bss_size=1)
    assert out == code[:4] + b"ok" + code[6:]
    with pytest.raises(code_mods.ModError):        # without the pages the source size/CRC doesn't fit
        code_mods.apply_mods(code, user, PID)


def _bps_commands(source, target):
    cmds, i = [], 0
    while i < len(target):
        same = source[i] == target[i]
        j = i
        while j < len(target) and (source[j] == target[j]) == same:
            j += 1
        cmds.append(("sr", j - i) if same else ("tr", target[i:j]))
        i = j
    return cmds


def test_replacement_then_patch_into_the_pages_and_unchanged_without_pages(tmp_path):
    replacement = bytes(range(100, 108))
    files = {"exefs/code.bin": replacement, "exefs/code.ips": ips((6, b"\xEE" * 4))}
    user = _mod_files(tmp_path, files)
    out, used = code_mods.apply_mods(b"ignored", user, PID, bss_size=0x2000)
    assert out == replacement[:6] + b"\xEE\xEE" and len(used) == 2
    # a mod that doesn't touch the pages gives the same bytes with or without them
    user2 = _mod_files(tmp_path / "c", {"code.ips": ips((1, b"\x09"))})
    assert code_mods.apply_mods(b"\1\2\3", user2, PID) == code_mods.apply_mods(b"\1\2\3", user2, PID, 0x500)
    assert code_mods.apply_mods(b"\1\2\3", user2, PID)[0] == b"\1\x09\3"
