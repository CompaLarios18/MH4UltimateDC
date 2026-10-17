"""N0-23: tools/native/service_commands.py on a synthetic services.tsv in the format
tools/find_services.py writes (COLUMNS at its line 2454, write_tsv at 2599). Every value here is made up."""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "native"))

import service_commands as sc  # noqa: E402

COLUMNS = ("function", "mode", "service", "header", "command", "callers", "site", "handle", "note")


def tsv(rows):
    lines = ["\t".join(COLUMNS)]
    for function, service, header, command in rows:
        lines.append("\t".join([function, "arm", service, header, command, "1", "0x00100004", "0x01000000", ""]))
    return "\n".join(lines) + "\n"


ROWS = [
    ("0x00100000", "srv:", "0x00050100", "GetServiceHandle"),
    ("0x00100100", "srv:", "0x00050100", "GetServiceHandle"),
    ("0x00100200", "fs:USER", "0x08010002", "Initialize"),
    ("0x00100300", "fs:USER", "0x080201c2", "OpenFile"),
    ("0x00100400", "cfg:u", "0x00010082", "GetConfigInfoBlk2"),
    ("0x00100500", "cfg:u", "0x00010082", "GetConfigInfoBlk2~"),
    ("0x00100600", "nwm::UDS?", "0x0017ffff", "?"),
    ("0x00100700", "http:C?", "0x00010044", "Initialize"),
    ("0x00100800", "", "0x00ff0000", "?"),
    ("0x00100900", "svc", "0x32", "SendSyncRequest"),
    ("0x00100a00", "gsp::Gpu", "?", "?"),
]


@pytest.fixture()
def tsv_file(tmp_path):
    p = tmp_path / "services.tsv"
    p.write_text(tsv(ROWS), encoding="utf-8", newline="\n")
    return p


def test_read_rows_skips_comments_and_blank(tmp_path):
    p = tmp_path / "s.tsv"
    p.write_text("# note\n\n" + tsv(ROWS[:2]), encoding="utf-8")
    rows = sc.read_rows(p)
    assert len(rows) == 2 and rows[0]["service"] == "srv:"


def test_missing_column_is_an_error(tmp_path):
    p = tmp_path / "s.tsv"
    p.write_text("function\tservice\nx\ty\n", encoding="utf-8")
    with pytest.raises(ValueError):
        sc.read_rows(p)


def test_counts_distinct_functions(tsv_file):
    c = sc.census(sc.read_rows(tsv_file))
    names, funcs = c["groups"]["traced"]["srv:"][0x00050100]
    assert names == {"GetServiceHandle"} and len(funcs) == 2


def test_groups_and_skips(tsv_file):
    c = sc.census(sc.read_rows(tsv_file))
    g = c["groups"]
    assert set(g["traced"]) == {"srv:", "fs:USER", "cfg:u"}   # gsp::Gpu has no constant header
    assert set(g["inferred"]) == {"nwm::UDS", "http:C"}
    assert set(g["untraced"]) == {""}
    assert c["unknown_header"] == 1          # the gsp::Gpu row has header '?'
    assert "svc" not in g["traced"]


def test_render_sections_and_rows(tsv_file):
    text = sc.render(sc.read_rows(tsv_file))
    assert text.endswith("\n") and "\r" not in text
    traced, rest = text.split("## Inferred services")
    inferred, untraced = rest.split("## Untraced requests")
    assert "| `0x00050100` | GetServiceHandle | 2 |" in traced
    assert "### `fs:USER`" in traced and "### `nwm::UDS`" not in traced
    assert "### `nwm::UDS` (inferred)" in inferred and "Not traced" in inferred
    assert "| `0x0017ffff` | ? | 1 |" in inferred
    assert "| `0x00ff0000` | ? | 1 |" in untraced
    assert "1 further request sites" in untraced
    # two names for one header: the ~ variant is kept distinct, both listed
    assert "GetConfigInfoBlk2, GetConfigInfoBlk2~" in traced


def test_render_is_deterministic_and_order_independent(tsv_file):
    rows = sc.read_rows(tsv_file)
    assert sc.render(rows) == sc.render(list(reversed(rows)))


def test_render_copies_nothing_but_headers_names_counts(tsv_file):
    text = sc.render(sc.read_rows(tsv_file))
    assert "0x00100004" not in text and "0x01000000" not in text and "0x00100000" not in text


def test_empty_groups_say_none():
    text = sc.render([])  # no rows at all
    assert text.count("None.") == 3


SESSION_MD = """intro
| Service | Handle global | Opened in | Notes |
|---|---|---|---|
| `srv:` | `0x01` | `0x02` | |
| `APT:U` (name chosen at run time) | `0x01` | `0x02` | |
| `cfg:u` (`cfg:s` and `cfg:i` are tried too) | `0x01` | `0x02` | |
| BOSS (SpotPass) | passed | not traced | |
| `nwm::UDS` | `0x01` | `0x02` | |

| Address | svc | Callers | Notes |
|---|---|---|---|
| `0x00105de8` | 0x01 ControlMemory | 2 | |
"""


def test_session_table_services():
    assert sc.session_table_services(SESSION_MD) == ["srv:", "APT:U", "cfg:u", "boss:U", "nwm::UDS"]


def test_missing_services(tsv_file):
    rows = sc.read_rows(tsv_file)
    # APT:U and boss:U are not in the synthetic census; inferred nwm::UDS counts as present
    assert sc.missing_services(rows, SESSION_MD) == ["APT:U", "boss:U"]


def test_main_writes_file_and_coverage_exit_code(tsv_file, tmp_path, capsys):
    out = tmp_path / "sub" / "service-commands.md"
    md = tmp_path / "system-services.md"
    md.write_text(SESSION_MD, encoding="utf-8")
    assert sc.main([str(tsv_file), "-o", str(out)]) == 0
    assert out.read_bytes().startswith(b"# Service commands") and b"\r" not in out.read_bytes()
    assert sc.main([str(tsv_file), "-o", str(out), "--coverage", str(md)]) == 1
    assert "APT:U" in capsys.readouterr().err
    assert sc.main([str(tmp_path / "nope.tsv")]) == 2
