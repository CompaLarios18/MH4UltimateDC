import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))

import build_function_table as bft  # noqa: E402
import exheader as exh  # noqa: E402

BASE = 0x00100000
CTOR = BASE + 0x100


def bl(at, target):
    return 0xEB000000 | (((target - (at + 8)) >> 2) & 0xFFFFFF)


def setup():
    buf = bytearray(0x200)
    words = {
        BASE + 0x00: 0xE92D4010, BASE + 0x04: 0xEF000032, BASE + 0x08: 0xE8BD8010,   # svc 0x32
        BASE + 0x20: 0xE92D4010, BASE + 0x24: bl(BASE + 0x24, CTOR), BASE + 0x28: 0xE8BD8010,
        BASE + 0x40: 0xE0800001, BASE + 0x44: 0xE12FFF1E,                            # add; bx lr
        CTOR: 0xE12FFF1E,
    }
    for a, w in words.items():
        struct.pack_into("<I", buf, a - BASE, w)
    buf[0x60:0x64] = bytes([0x32, 0xDF, 0x70, 0x47])                                # thumb: svc 0x32; bx lr
    seg = exh.CodeSegment
    header = exh.ExHeader("T", 0, 0, seg(BASE, 1, 0x200), 0, seg(BASE + 0x1000, 0, 0), seg(BASE + 0x1000, 0, 0), 0, None)
    funcs = [{"address": f"0x{a:08x}", "size": hex(s), "mode": m}
             for a, s, m in [(BASE, 0xC, "arm"), (BASE + 0x20, 0xC, "arm"), (BASE + 0x40, 8, "arm"),
                             (BASE + 0x60, 4, "thumb"), (CTOR, 4, "arm")]]
    return bytes(buf), header, funcs


def test_categories_and_names():
    code, header, funcs = setup()
    rows = bft.build(code, header, funcs, {BASE + 0x40: "add_two"}, CTOR, {})
    by = {int(r["address"], 16): r for r in rows}
    assert by[BASE]["category"] == "sdk" and by[BASE]["status"] == "replace"
    assert by[BASE + 0x20]["category"] == "dti-init"
    assert by[BASE + 0x40]["category"] == "unknown" and by[BASE + 0x40]["name"] == "add_two"
    assert by[BASE + 0x60]["category"] == "sdk"
    assert by[CTOR]["category"] == "unknown"


def test_existing_status_is_preserved(tmp_path):
    code, header, funcs = setup()
    existing = {BASE + 0x40: {"status": "done", "batch": "pilot-1", "notes": "ok"}}
    rows = bft.build(code, header, funcs, {}, CTOR, existing)
    row = next(r for r in rows if int(r["address"], 16) == BASE + 0x40)
    assert (row["status"], row["batch"], row["notes"]) == ("done", "pilot-1", "ok")
    out = tmp_path / "functions.tsv"
    bft.write_tsv(rows, str(out))
    assert bft.read_tsv(str(out))[2]["batch"] == "pilot-1"


def test_default_status_follows_a_new_category():
    # a rebuild that newly finds a function sdk makes it replace, even though the old table's
    # status was the default todo; a status someone set stays
    code, header, funcs = setup()
    existing = {BASE: {"status": "todo"}, BASE + 0x60: {"status": "blocked"}, BASE + 0x40: {"status": "replace"}}
    rows = bft.build(code, header, funcs, {}, CTOR, existing)
    by = {int(r["address"], 16): r for r in rows}
    assert by[BASE]["status"] == "replace"
    assert by[BASE + 0x60]["status"] == "blocked"
    assert by[BASE + 0x40]["category"] == "unknown" and by[BASE + 0x40]["status"] == "todo"


def test_load_symbols(tmp_path):
    p = tmp_path / "symbols.txt"
    p.write_text("# comment\n0x00100000 entry   # note\n\n0x005bfa94 MtDTI::MtDTI\n")
    assert bft.load_symbols(str(p)) == {0x00100000: "entry", 0x005BFA94: "MtDTI::MtDTI"}


def test_fnstatus_set_and_show(tmp_path, capsys):
    import fnstatus
    code, header, funcs = setup()
    table = tmp_path / "functions.tsv"
    bft.write_tsv(bft.build(code, header, funcs, {}, CTOR, {}), str(table))
    fnstatus.main(["--table", str(table), "set", hex(BASE + 0x40), "--status", "done",
                   "--batch", "pilot-0", "--name", "add_two"])
    row = next(r for r in bft.read_tsv(str(table)) if int(r["address"], 16) == BASE + 0x40)
    assert (row["status"], row["batch"], row["name"]) == ("done", "pilot-0", "add_two")
    fnstatus.main(["--table", str(table), "summary"])
    assert "decompiled bytes: 0x8" in capsys.readouterr().out


def test_sdk_names_match_exactly_or_by_runtime_prefix():
    code, header, funcs = setup()
    names = ["__aeabi_memcpy", "memset", "nn::os::Sleep", "freeList", "exitQuest", "_ZN6sStart4moveEv", "svcHandler"]
    extra = [BASE + 0x80 + 4 * i for i in range(len(names))]
    funcs += [{"address": f"0x{a:08x}", "size": "0x4", "mode": "arm"} for a in extra]
    rows = bft.build(code, header, funcs, dict(zip(extra, names)), CTOR, {})
    got = {r["name"]: (r["category"], r["status"]) for r in rows if r["name"]}
    assert got["__aeabi_memcpy"] == got["memset"] == got["nn::os::Sleep"] == ("sdk", "replace")
    assert got["freeList"] == got["exitQuest"] == got["_ZN6sStart4moveEv"] == got["svcHandler"] == ("unknown", "todo")


def propagation_case():
    """svc at A; B calls A; C calls A and B; D calls A and an unknown function; E calls the DTI
    constructor only (dti-init itself) and F calls E."""
    buf = bytearray(0x200)
    a, b, c, d, e, f, leaf = (BASE + 0x20 * i for i in range(7))
    words = {
        a: 0xE92D4010, a + 4: 0xEF000032, a + 8: 0xE8BD8010,
        b: 0xE92D4010, b + 4: bl(b + 4, a), b + 8: 0xE8BD8010,
        c: 0xE92D4010, c + 4: bl(c + 4, a), c + 8: bl(c + 8, b), c + 12: 0xE8BD8010,
        d: 0xE92D4010, d + 4: bl(d + 4, a), d + 8: bl(d + 8, leaf), d + 12: 0xE8BD8010,
        e: 0xE92D4010, e + 4: bl(e + 4, CTOR), e + 8: 0xE8BD8010,
        f: 0xE92D4010, f + 4: bl(f + 4, e), f + 8: 0xE8BD8010,
        leaf: 0xE12FFF1E, CTOR: 0xE12FFF1E,
    }
    for at, w in words.items():
        struct.pack_into("<I", buf, at - BASE, w)
    seg = exh.CodeSegment
    header = exh.ExHeader("T", 0, 0, seg(BASE, 1, 0x200), 0, seg(BASE + 0x1000, 0, 0), seg(BASE + 0x1000, 0, 0), 0, None)
    funcs = [{"address": f"0x{x:08x}", "size": "0x10", "mode": "arm"} for x in (a, b, c, d, e, f, leaf, CTOR)]
    return bytes(buf), header, funcs, (a, b, c, d, e, f, leaf)


def test_propagation_is_off_by_default():
    code, header, funcs, (a, b, c, *_rest) = propagation_case()
    by = {int(r["address"], 16): r["category"] for r in bft.build(code, header, funcs, {}, CTOR, {})}
    assert by[a] == "sdk" and by[b] == by[c] == "unknown"


def test_propagation_follows_calls_that_all_go_to_sdk_functions():
    code, header, funcs, (a, b, c, d, e, f, leaf) = propagation_case()
    existing = {c: {"status": "blocked"}}
    rows = bft.build(code, header, funcs, {}, CTOR, existing, propagate_sdk=True)
    by = {int(r["address"], 16): r for r in rows}
    assert by[b]["category"] == "sdk" and by[b]["status"] == "replace"
    assert by[c]["category"] == "sdk" and by[c]["status"] == "blocked"   # the table's status is kept
    assert by[d]["category"] == "unknown"                                 # one call goes to game code
    assert by[e]["category"] == "dti-init" and by[f]["category"] == "unknown"
    assert by[leaf]["category"] == "unknown"                              # no call: nothing to go on
