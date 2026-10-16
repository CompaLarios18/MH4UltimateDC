"""tools/native/wp_status.py: work-package specs and docs/native/STATUS.md (synthetic specs)."""
import os
import sys

import pytest

HERE = os.path.dirname(__file__)
ROOT = os.path.join(HERE, "..")
sys.path.insert(0, os.path.join(ROOT, "tools", "native"))

import wp_status as ws  # noqa: E402

SPEC_A = """# N9 · test phase

### N9-01 · First thing
- **Size / agent:** S · native-worker
- **Depends on:** - · **Needs:** none
- **Goal:** x
- **Files (allowlist):** `tools/native/first.py` (new), `tests/test_first.py` (new).

### N9-02 · Second thing
- **Size / agent:** M · native-worker-high (split if needed)
- **Depends on:** N9-01 · **Needs:** ci, dump-check
- **Files (allowlist):** `native/src/second/**`, `native/tests/second_test.cpp`.

### N9-03 · Third thing
- **Size / agent:** L · native-worker, then the maintainer
- **Depends on:** N9-01 to N9-02 (both) · **Needs:** pc (the real one), dump
- **Files (allowlist):** `native/src/**` (fixes), `tools/native/first.py`.
"""

SPEC_B = """# N8 · another phase

### N8-01 · Pipes | and bars
- **Size / agent:** S · native-worker
- **Depends on:** N9-03 · **Needs:** -
- **Files (allowlist):** -
"""

STATUS = """# Status

<!-- wp-status:begin N9 -->
<!-- wp-status:end N9 -->

## Maintainer tasks
- keep this line
"""


def write_tree(tmp_path, spec=SPEC_A, status=STATUS, extra=None):
    wp = tmp_path / "wp"
    wp.mkdir()
    (wp / "N9.md").write_text(spec, encoding="utf-8")
    for name, text in (extra or {}).items():
        (wp / name).write_text(text, encoding="utf-8")
    (wp / "LATER.md").write_text("### N9-99 · ignored\n", encoding="utf-8")
    st = tmp_path / "STATUS.md"
    st.write_text(status, encoding="utf-8")
    return str(wp), str(st)


def run(tmp_path, *args):
    wp, st = str(tmp_path / "wp"), str(tmp_path / "STATUS.md")
    return ws.main(["--wp-dir", wp, "--status-file", st, *args])


def status_text(tmp_path):
    return (tmp_path / "STATUS.md").read_text(encoding="utf-8")


def row(tmp_path, wp):
    return next(line for line in status_text(tmp_path).splitlines() if line.startswith(f"| {wp} |"))


def test_parse_depends_forms():
    assert ws.parse_depends("-") == []
    assert ws.parse_depends("N0-03, N0-04") == ["N0-03", "N0-04"]
    assert ws.parse_depends("N1-12 to N1-14") == ["N1-12", "N1-13", "N1-14"]
    assert ws.parse_depends("N2-04, N2-01 (D4)") == ["N2-04", "N2-01"]
    assert ws.parse_depends("N2b-01") == ["N2b-01"]
    for bad in ("N1-01 to N2-03", "N1-15 to N1-12", "N1-03 and N1-07", "N1-03 maybe"):
        with pytest.raises(ws.SpecError):
            ws.parse_depends(bad)


def test_parse_needs_and_agent():
    assert ws.parse_needs("ci, dump-check") == ["ci", "dump-check"]
    assert ws.parse_needs("none (it uses the reference runner artifact)") == ["none"]
    assert ws.parse_needs("dump-check, 3ds (gas, later)") == ["dump-check", "3ds"]
    assert ws.parse_needs("-") == ["none"]
    with pytest.raises(ws.SpecError):
        ws.parse_needs("pc and mac")
    assert ws.parse_agent("native-worker-high (split)") == "native-worker-high"
    assert ws.parse_agent("native-worker, then the maintainer") == "native-worker"
    with pytest.raises(ws.SpecError):
        ws.parse_agent("someone")


def test_parse_specs_skips_later_and_reads_two_files(tmp_path):
    wp, _ = write_tree(tmp_path, extra={"N8.md": SPEC_B})
    packages, errors = ws.parse_specs(ws.spec_paths(wp))
    assert errors == []
    assert [p.id for p in packages] == ["N8-01", "N9-01", "N9-02", "N9-03"]
    third = packages[3]
    assert (third.size, third.agent, third.depends, third.needs) == ("L", "native-worker", ["N9-01", "N9-02"], ["pc", "dump"])
    assert packages[0].phase == "N8" and packages[0].needs == ["none"]


def test_parse_errors_carry_file_and_line(tmp_path):
    bad = SPEC_A.replace("**Depends on:** N9-01 · **Needs:** ci, dump-check",
                         "**Depends on:** N9-01 and N9-03 · **Needs:** ci, dump-check")
    bad += "\n### N9-04a split package\n"
    wp, _ = write_tree(tmp_path, spec=bad)
    _, errors = ws.parse_specs(ws.spec_paths(wp))
    assert any(e.endswith("N9-02: can't read dependency 'N9-01 and N9-03' (write IDs separated by commas, or 'A to B')")
               and "N9.md:" in e for e in errors)
    assert any("heading not recognised" in e for e in errors)
    run(tmp_path, "sync")   # refuses while the specs have errors
    assert "| N9-01 |" not in status_text(tmp_path)
    assert run(tmp_path, "check") == 1


def test_sync_builds_tables_and_keeps_the_rest(tmp_path):
    write_tree(tmp_path)
    assert run(tmp_path, "sync") == 0
    text = status_text(tmp_path)
    assert "| N9-01 | First thing | S | native-worker | - | none | `ready` |  |" in text
    assert "| N9-02 | Second thing | M | native-worker-high | N9-01 | ci, dump-check | `todo` |  |" in text
    assert "- keep this line" in text
    assert run(tmp_path, "check") == 0


def test_sync_adds_a_table_for_a_new_phase(tmp_path):
    write_tree(tmp_path, extra={"N8.md": SPEC_B})
    assert run(tmp_path, "sync") == 0
    text = status_text(tmp_path)
    assert "<!-- wp-status:begin N8 -->" in text and "<!-- wp-status:end N8 -->" in text
    assert text.index("wp-status:end N9") < text.index("## N8") < text.index("## Maintainer tasks")
    assert r"| N8-01 | Pipes \| and bars | S | native-worker | N9-03 | none | `todo` |  |" in text
    assert run(tmp_path, "check") == 0


def test_check_reports_a_phase_without_table(tmp_path, capsys):
    write_tree(tmp_path, extra={"N8.md": SPEC_B})
    capsys.readouterr()
    assert run(tmp_path, "check") == 1
    assert "phase N8: no wp-status table" in capsys.readouterr().out


def test_unbalanced_markers_are_an_error(tmp_path, capsys):
    write_tree(tmp_path, status=STATUS.replace("<!-- wp-status:end N9 -->\n", ""))
    capsys.readouterr()
    assert run(tmp_path, "sync") == 1
    assert "has no end marker" in capsys.readouterr().out
    assert "keep this line" in status_text(tmp_path)


SPEC_C = """# N7 · ranking

### N7-01 · Leaf
- **Size / agent:** S · native-worker
- **Depends on:** - · **Needs:** none
- **Files (allowlist):** `a.txt`

### N7-02 · Root of two
- **Size / agent:** S · native-worker
- **Depends on:** - · **Needs:** dump
- **Files (allowlist):** `b.txt`

### N7-03 · Needs the save for its checks only
- **Size / agent:** S · native-worker
- **Depends on:** - · **Needs:** dump-check, save
- **Files (allowlist):** `c.txt`

### N7-04 · Needs the save for its work
- **Size / agent:** S · native-worker
- **Depends on:** - · **Needs:** save
- **Files (allowlist):** `d.txt`

### N7-05 · After the root
- **Size / agent:** S · native-worker
- **Depends on:** N7-02 · **Needs:** none
- **Files (allowlist):** `e.txt`

### N7-06 · After both
- **Size / agent:** S · native-worker
- **Depends on:** N7-05 · **Needs:** none
- **Files (allowlist):** `f.txt`
"""


def ready_ids(tmp_path, capsys, *args):
    capsys.readouterr()
    assert run(tmp_path, "ready", *args) == 0
    return [line.split("\t")[0] for line in capsys.readouterr().out.splitlines()]


def test_ready_follows_dependencies(tmp_path, capsys):
    write_tree(tmp_path)
    run(tmp_path, "sync")
    assert run(tmp_path, "set", "N9-01", "done", "--note", "PR #3") == 0
    assert row(tmp_path, "N9-01") == "| N9-01 | First thing | S | native-worker | - | none | `done` | PR #3 |"
    assert "`ready`" in row(tmp_path, "N9-02")
    capsys.readouterr()
    run(tmp_path, "ready")
    out = capsys.readouterr().out
    assert out.startswith("N9-02\tM\tnative-worker-high\tci,dump-check\tunblocks=1\tSecond thing")


def test_ready_ranks_by_what_it_unblocks_and_filters_game_files(tmp_path, capsys):
    status = STATUS.replace("N9", "N7")
    write_tree(tmp_path, spec=SPEC_C, status=status)
    (tmp_path / "wp" / "N9.md").rename(tmp_path / "wp" / "N7.md")
    assert run(tmp_path, "sync") == 0
    assert ready_ids(tmp_path, capsys) == ["N7-02", "N7-01", "N7-03", "N7-04"]
    # --no-dump: N7-02 needs the dump, N7-04 the save for its work; N7-03 only for its checks
    assert ready_ids(tmp_path, capsys, "--no-dump") == ["N7-01", "N7-03"]
    # a package merged into this session's batch counts as done for what follows it
    assert ready_ids(tmp_path, capsys, "--assume-done", "N7-02") == ["N7-05", "N7-01", "N7-03", "N7-04"]
    # several IDs, with commas or spaces
    assert ready_ids(tmp_path, capsys, "--assume-done", "N7-02,N7-05") == ["N7-01", "N7-03", "N7-04", "N7-06"]
    assert ready_ids(tmp_path, capsys, "--assume-done", "N7-02", "N7-05") == ["N7-01", "N7-03", "N7-04", "N7-06"]
    assert run(tmp_path, "ready", "--assume-done", "N7-99") == 1
    # merged into the batch but waiting for a decision: what follows it waits too
    run(tmp_path, "set", "N7-02", "review", "--note", "batch 20261016; waiting for D4")
    assert ready_ids(tmp_path, capsys, "--assume-done", "N7-02") == ["N7-01", "N7-03", "N7-04"]


def test_parse_files_forms():
    line = ("`tools/native/golden.py` (new), `tools/native/golden/G1.txt`, `G2.txt` (new), "
            "`native/runner/**` (`--print-process`, its `CMakeLists.txt`), "
            "`native/src/services/{am,nim}/**`, `native/third_party/<BC7 encoder>/**`, "
            "`.github/workflows/native.yml` (a `native` job).")
    assert ws.parse_files(line) == [
        "tools/native/golden.py", "tools/native/golden/G1.txt", "tools/native/golden/G2.txt",
        "native/runner/**", "native/src/services/am/**", "native/src/services/nim/**",
        "native/third_party/**", ".github/workflows/native.yml"]
    assert ws.parse_files("`docs/native/**`, `./CLAUDE.md`, `tools/x.py`, `y.py`") == [
        "docs/native/**", "CLAUDE.md", "tools/x.py", "tools/y.py"]
    assert ws.parse_files("-") == []


def test_patterns_overlap():
    yes = [("a/b.txt", "a/b.txt"), ("native/**", "native/src/x/y.cpp"), ("native/src/gpu/**", "native/src/gpu/gx/**"),
           ("native/tests/*_test.cpp", "native/tests/memory_test.cpp"), ("docs/native/**", "docs/native/ENV.md")]
    yes += [("native/src/cpu/**", "native/src/cpu/interp/**"), ("native/tests/*_test.cpp", "native/tests/gpu_*.cpp")]
    no = [("a/b.txt", "a/c.txt"), ("native/src/gpu/**", "native/src/gpu_null/**"),
          ("port/tests/conf_*.c", "port/tests/expected/*.txt"), ("native/tests/*_test.cpp", "native/tests/data.bin"),
          ("native/src/cpu/*", "native/src/cpu/interp/**"), ("native/src/gpu/**", "native/src/gpu")]
    for a, b in yes:
        assert ws.patterns_overlap(a, b) and ws.patterns_overlap(b, a), (a, b)
    for a, b in no:
        assert not ws.patterns_overlap(a, b) and not ws.patterns_overlap(b, a), (a, b)


def test_overlaps_lists_pairs_that_can_run_together(tmp_path, capsys):
    write_tree(tmp_path)
    run(tmp_path, "sync")
    capsys.readouterr()
    # N9-03 depends on both others, so with --all nothing can collide
    assert run(tmp_path, "overlaps", "--all") == 0
    assert capsys.readouterr().out == ""
    # asked about explicitly, the shared files show
    assert run(tmp_path, "overlaps", "N9-01", "N9-02", "N9-03") == 1
    assert capsys.readouterr().out.splitlines() == [
        "N9-01 N9-03: tools/native/first.py",
        "N9-02 N9-03: native/src/second/** ~ native/src/**"]
    assert run(tmp_path, "overlaps", "N9-01") == 2
    capsys.readouterr()
    assert run(tmp_path, "overlaps", "N9-01,N9-03") == 1     # commas work too
    assert capsys.readouterr().out.splitlines() == ["N9-01 N9-03: tools/native/first.py"]
    # with --all, done packages and the ones assumed done drop out
    spec = SPEC_A.replace("**Depends on:** N9-01 to N9-02 (both) · **Needs:** pc (the real one), dump",
                          "**Depends on:** - · **Needs:** pc (the real one), dump")
    (tmp_path / "wp" / "N9.md").write_text(spec, encoding="utf-8")
    run(tmp_path, "sync")
    capsys.readouterr()
    assert run(tmp_path, "overlaps", "--all") == 1
    assert len(capsys.readouterr().out.splitlines()) == 2
    assert run(tmp_path, "overlaps", "--all", "--assume-done", "N9-03") == 0
    run(tmp_path, "set", "N9-03", "done")
    capsys.readouterr()
    assert run(tmp_path, "overlaps", "--all") == 0


def test_check_wants_a_files_line(tmp_path, capsys):
    write_tree(tmp_path, spec=SPEC_A.replace("- **Files (allowlist):** `tools/native/first.py` (new), `tests/test_first.py` (new).\n", ""))
    run(tmp_path, "sync")
    capsys.readouterr()
    assert run(tmp_path, "check") == 1
    assert "N9-01: no 'Files (allowlist)' line" in capsys.readouterr().out


def test_a_dependency_in_review_is_not_done(tmp_path):
    write_tree(tmp_path)
    run(tmp_path, "sync")
    run(tmp_path, "set", "N9-01", "review", "--note", "PR #4")
    assert "`todo`" in row(tmp_path, "N9-02")


def test_set_keeps_the_note_and_escapes_bars(tmp_path, capsys):
    write_tree(tmp_path)
    run(tmp_path, "sync")
    run(tmp_path, "set", "N9-02", "review", "--note", "batch 20261016 | dump-check pending")
    run(tmp_path, "set", "N9-02", "done")
    assert row(tmp_path, "N9-02").endswith(r"| `done` | batch 20261016 \| dump-check pending |")
    assert ws.parse_status(status_text(tmp_path))["N9-02"].note == "batch 20261016 | dump-check pending"
    run(tmp_path, "sync")
    assert ws.parse_status(status_text(tmp_path))["N9-02"].status == "done"
    capsys.readouterr()
    run(tmp_path, "pending")
    assert capsys.readouterr().out.startswith("N9-02\tdone\t")


def test_add_note_goes_in_front(tmp_path):
    write_tree(tmp_path)
    run(tmp_path, "sync")
    run(tmp_path, "set", "N9-02", "todo", "--note", "WIP on origin/wp/N9-02-thing")
    run(tmp_path, "set", "N9-02", "active", "--add-note", "session 20261016")
    assert row(tmp_path, "N9-02").endswith("| `active` | session 20261016; WIP on origin/wp/N9-02-thing |")
    run(tmp_path, "set", "N9-01", "active", "--add-note", "session 20261016")
    assert row(tmp_path, "N9-01").endswith("| `active` | session 20261016 |")
    with pytest.raises(SystemExit):
        run(tmp_path, "set", "N9-01", "done", "--note", "a", "--add-note", "b")


def test_pending_lists_every_owed_marker(tmp_path, capsys):
    write_tree(tmp_path)
    run(tmp_path, "sync")
    run(tmp_path, "set", "N9-01", "review", "--note", "PR #7; waiting for D5")
    run(tmp_path, "set", "N9-02", "todo", "--note", "WIP on origin/wp/N9-02-thing: budget")
    run(tmp_path, "set", "N9-03", "review", "--note", "PR #7")
    capsys.readouterr()
    assert run(tmp_path, "pending") == 0
    lines = capsys.readouterr().out.splitlines()
    assert [ln.split("\t")[0] for ln in lines] == ["N9-01", "N9-02"]


def test_check_reports_problems(tmp_path, capsys):
    bad = SPEC_A.replace("**Depends on:** N9-01 · **Needs:** ci, dump-check",
                         "**Depends on:** N9-03, N9-09 · **Needs:** ci, gpu")
    write_tree(tmp_path, spec=bad)
    run(tmp_path, "sync")
    st = tmp_path / "STATUS.md"
    st.write_text(st.read_text(encoding="utf-8").replace("| none | `ready` |", "| none | `waiting` |"),
                  encoding="utf-8")
    capsys.readouterr()
    assert run(tmp_path, "check") == 1
    out = capsys.readouterr().out
    assert "N9-02: depends on unknown N9-09" in out
    assert "N9-02: unknown need 'gpu'" in out
    assert "cycle: N9-02 → N9-03 → N9-02" in out or "cycle: N9-03 → N9-02 → N9-03" in out
    assert "N9-01: unknown status 'waiting'" in out


def test_check_notices_a_stale_table(tmp_path, capsys):
    write_tree(tmp_path)
    run(tmp_path, "sync")
    (tmp_path / "wp" / "N9.md").write_text(SPEC_A.replace("Third thing", "Third thing, renamed"), encoding="utf-8")
    capsys.readouterr()
    assert run(tmp_path, "check") == 1
    assert "not in sync" in capsys.readouterr().out


def test_set_unknown_package_or_status(tmp_path):
    write_tree(tmp_path)
    run(tmp_path, "sync")
    assert run(tmp_path, "set", "N9-42", "done") == 1
    assert run(tmp_path, "set", "N9-01", "finished") == 1


def test_repository_status_is_consistent():
    if not os.path.exists(ws.STATUS_PATH):
        pytest.skip("docs/native/STATUS.md not present")
    assert ws.main(["check"]) == 0
