import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))

import exheader as exh  # noqa: E402
import version_map as vm  # noqa: E402

BASE = 0x00100000
PUSH, POP, NOP = 0xE92D4010, 0xE8BD8010, 0xE320F000


def bl(at, target):
    return 0xEB000000 | (((target - (at + 8)) >> 2) & 0xFFFFFF)


def b(at, target):
    return 0xEA000000 | (((target - (at + 8)) >> 2) & 0xFFFFFF)


def thumb_bl(at, target):
    off = target - (at + 4)
    return [0xF000 | ((off >> 12) & 0x7FF), 0xF800 | ((off >> 1) & 0x7FF)]


def build(insert=0, bad_call=False):
    """Five ARM functions and a Thumb one. `insert` extra words go into f1 (a function the
    other version changed), moving everything after it."""
    f = [BASE, BASE + 0x40, BASE + 0x80 + insert * 4, BASE + 0xC0 + insert * 4, BASE + 0x100 + insert * 4]
    t = BASE + 0x140 + insert * 4
    words = {}

    def put(at, ws):
        for i, w in enumerate(ws):
            words[at + 4 * i] = w

    put(f[0], [PUSH, 0xE3A00001, bl(f[0] + 8, f[2]), bl(f[0] + 12, f[3]), POP])
    put(f[1], [PUSH, 0xE3A00002] + [0xE2800001] * insert + [0xE3A01002, POP])
    put(f[2], [PUSH, 0xE3A00003, bl(f[2] + 8, f[0] if bad_call else f[4]), 0xE59F0004, POP, NOP, t | 1])
    put(f[3], [PUSH, 0xE3A00004, 0xE3A01004, 0xE3A02004, b(f[3] + 16, f[4])])
    put(f[4], [PUSH, 0xE3A00005, 0xE3A01005, 0xE3A02005, POP])
    buf = bytearray(0x200 + insert * 4)
    for a, w in words.items():
        struct.pack_into("<I", buf, a - BASE, w)
    halves = [0xB510, 0x2006, 0x2106, *thumb_bl(t + 6, t + 0x10), 0x2206, 0xBD10, 0x46C0,
              0xB510, 0x2007, 0x2107, 0x2207, 0x2307, 0xBD10]
    for i, h in enumerate(halves):
        struct.pack_into("<H", buf, t - BASE + 2 * i, h)
    image = vm.Image(BASE, bytes(buf), BASE + len(buf))
    funcs = [vm.Function(a, 0x14 if a != f[2] else 0x1C, False) for a in f]
    funcs[1] = vm.Function(f[1], 0x10 + insert * 4, False)
    funcs += [vm.Function(t, 0x10, True), vm.Function(t + 0x10, 0xC, True)]
    return image, funcs, f, t


def test_shifted_functions_map_and_the_changed_one_does_not():
    src, funcs, f, t = build()
    dst, _, g, u = build(insert=2)
    mapped, report = vm.match_functions(src, dst, funcs)
    assert mapped[f[0]] == g[0]
    assert f[1] not in mapped
    assert [mapped[a] for a in f[2:]] == g[2:]
    assert mapped[t] == u and mapped[t + 0x10] == u + 0x10
    assert report["cross_checked"] >= 5 and report["dropped_inconsistent"] == 0


def test_shifts_translate_inside_functions_and_leave_other_segments():
    src, funcs, f, t = build()
    dst, _, g, u = build(insert=2)
    shifts, report = vm.build_shifts(src, dst, funcs)
    assert shifts.translate(f[3] + 8) == g[3] + 8
    assert shifts.translate(t | 1) == u | 1                      # the Thumb bit is kept
    assert shifts.translate(f[1] + 4) is None                     # the changed function
    assert shifts.translate(0x00EC0010) == 0x00EC0010             # outside .text: same address
    assert shifts.translate(f[0]) == f[0]


def test_a_call_to_the_wrong_place_drops_its_function():
    src, funcs, f, _ = build()
    dst, _, g, _ = build(insert=2, bad_call=True)
    mapped, report = vm.match_functions(src, dst, funcs)
    assert f[2] not in mapped
    assert mapped[f[3]] == g[3]


def test_round_trip_and_translate_keys():
    shifts = vm.Shifts([(BASE, BASE + 0x40, 0), (BASE + 0x80, BASE + 0x200, 8)], (BASE, BASE + 0x200), ["hello"])
    again = vm.Shifts.loads(shifts.dumps())
    assert again.intervals == shifts.intervals and again.text == shifts.text and again.header == ["hello"]
    out, missing = again.translate_keys({BASE + 4: "a", BASE + 0x44: "b", BASE + 0x84: "c"})
    assert out == {BASE + 4: "a", BASE + 0x8C: "c"}
    assert missing == [BASE + 0x44]


def test_layout_outside_text_must_match():
    seg = exh.CodeSegment
    a = exh.ExHeader("T", 0, 0, seg(BASE, 2, 0x1000), 0, seg(0x200000, 1, 0x100), seg(0x300000, 1, 0x100), 0x10, None)
    b = exh.ExHeader("T", 0, 0, seg(BASE, 2, 0x1008), 0, seg(0x200000, 1, 0x100), seg(0x300000, 1, 0x100), 0x10, None)
    c = exh.ExHeader("T", 0, 0, seg(BASE, 2, 0x1008), 0, seg(0x200000, 1, 0x104), seg(0x300000, 1, 0x100), 0x10, None)
    assert vm.same_layout(a, b) == []
    assert vm.same_layout(a, c) == [".ro: 0x00200000+0x100 vs 0x00200000+0x104"]


def test_translate_command(tmp_path, capsys):
    path = tmp_path / "shifts.txt"
    path.write_text(vm.Shifts([(BASE, BASE + 0x40, 0x20)], (BASE, BASE + 0x100)).dumps())
    assert vm.main(["translate", str(path), "0x00100010", "0x00100050"]) == 0
    assert capsys.readouterr().out.splitlines() == ["0x00100010 -> 0x00100030", "0x00100050 -> unmapped"]
