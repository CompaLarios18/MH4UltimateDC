"""tools/mods.py's catalog and installs: downloading (resumed, checked), unpacking .zip, .7z
and .tar safely, texture packs into Azahar's texture folder and out again, mods into the
library, the catalog that comes with the tools and a newer one from the web. All with
synthetic files served from a temporary folder or a local HTTP server (never game data, never
the network)."""
import contextlib
import hashlib
import http.server
import io
import json
import os
import sys
import tarfile
import threading
import zipfile

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
sys.path.insert(0, HERE)
import mods  # noqa: E402
from sevenzip_writer import make_7z  # noqa: E402


@pytest.fixture(autouse=True)
def usa_game(monkeypatch):
    """Each test starts on the USA game: main (--program-id, or the update installed) and
    set_game change these module globals, which monkeypatch puts back after the test."""
    monkeypatch.setattr(mods, "PROGRAM_ID", 0x0004000000126300)
    monkeypatch.setattr(mods, "UPDATE_ID", 0x0004000E00126300)


TID = "0004000000126300"


def write(path, data=b"x"):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data if isinstance(data, bytes) else data.encode())
    return path


def make_zip(path, entries):
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in entries.items():
            z.writestr(name, data)
    return path


def call(user, *args, code=0):
    """mods.py --json: (its result, its progress events)."""
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = mods.main(["--json", "--user-dir", str(user), *args])
    lines = buf.getvalue().splitlines(keepends=True)
    events = [json.loads(line[1:]) for line in lines if line.startswith("@")]
    out = json.loads("".join(line for line in lines if not line.startswith("@")))
    assert rc == code, out
    assert out["ok"] is (code == 0)
    return out, events


def run(user, *args, code=0):
    return call(user, *args, code=code)[0]


def textures(user):
    return user / "load" / "textures" / TID


def tree(top):
    return sorted(str(p.relative_to(top)).replace(os.sep, "/") for p in top.rglob("*") if p.is_file())


@pytest.fixture
def user(tmp_path):
    u = tmp_path / "azahar"
    u.mkdir()
    return u


def catalog(path, entries, updated="2026-01-01"):
    path.write_text(json.dumps({"format": 1, "updated": updated, "mods": entries}))
    return str(path)


def entry(cid, kind, downloads, **more):
    return dict({"id": cid, "kind": kind, "name": cid.replace("-", " ").title(), "version": "1",
                 "author": "someone", "description": "test", "page_url": f"https://example.org/{cid}",
                 "downloads": downloads}, **more)


def url(path):
    return path.as_uri()


class Server:
    """A local HTTP server for {path: bytes}, with Range requests (or without, to test that)."""

    def __init__(self, files, ranges=True):
        self.files, self.ranges, self.requests = files, ranges, []
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                data = outer.files.get(self.path)
                rng = self.headers.get("Range")
                outer.requests.append((self.path, rng))
                if data is None:
                    self.send_error(404)
                    return
                start = 0
                if rng and outer.ranges:
                    start = int(rng.split("=")[1].split("-")[0])
                    self.send_response(206)
                    self.send_header("Content-Range", f"bytes {start}-{len(data) - 1}/{len(data)}")
                else:
                    self.send_response(200)
                body = data[start:]
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass

        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.base = f"http://127.0.0.1:{self.httpd.server_address[1]}"
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


@pytest.fixture
def server():
    servers = []

    def start(files, ranges=True):
        s = Server(files, ranges)
        servers.append(s)
        return s
    yield start
    for s in servers:
        s.close()


# ── the catalog that comes with the tools ──

def test_bundled_catalog_is_valid():
    with open(mods.CATALOG, encoding="utf-8") as f:
        raw = json.load(f)
    cat, warnings = mods.validate_catalog(raw, remote=True)    # as strict as one from the web
    assert warnings == []
    assert len(cat["mods"]) == len(raw["mods"]) >= 5
    ids = [e["id"] for e in cat["mods"]]
    assert len(set(ids)) == len(ids)
    recommended = [e for e in cat["mods"] if e["recommended"]]
    assert [e["kind"] for e in recommended] == ["textures"]           # the HD texture pack
    assert "custom_textures" in recommended[0]["needs"]
    for e in cat["mods"]:
        assert e["page_url"].startswith("https://") and e["author"] and e["description"] and e["license"]
        assert e["region"] in {name for _, name in mods.REGIONS.values()}, e["id"]   # as fits compares it
        for c in e["conflicts"]:
            assert c in ids
        for d in e["downloads"]:
            assert d["url"].startswith("https://")


def test_catalog_lists_with_install_state(user, tmp_path):
    out = run(user, "catalog", "--catalog-url", "")
    assert out["catalog"]["source"] == "bundled"
    assert out["textures_folder"] == str(textures(user))
    hd = next(e for e in out["entries"] if e["recommended"])
    assert hd["installed"] is None and hd["direct"] and hd["size"] > 10 << 30
    # text output, too
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        assert mods.main(["--user-dir", str(user), "catalog", "--catalog-url", ""]) == 0
    assert "[recommended]" in buf.getvalue()


def test_bad_entries_are_left_out():
    raw = {"updated": "x", "mods": [
        {"id": "ok", "kind": "mod", "name": "Ok", "page_url": "https://e.org", "downloads": []},
        {"id": "ok", "kind": "mod", "name": "Again", "page_url": "https://e.org"},          # repeated id
        {"id": "Bad Id", "kind": "mod", "name": "x", "page_url": "https://e.org"},
        {"id": "http", "kind": "mod", "name": "x", "page_url": "https://e.org",
         "downloads": [{"url": "http://e.org/x.zip"}]},                                       # not https
        {"id": "escape", "kind": "textures", "name": "x", "page_url": "https://e.org",
         "downloads": [{"url": "https://e.org/x.zip", "into": "../../x"}]},
        {"id": "kind", "kind": "game", "name": "x", "page_url": "https://e.org"},
        {"id": "sum", "kind": "mod", "name": "x", "page_url": "https://e.org",
         "downloads": [{"url": "https://e.org/x.zip", "sha256": "abc"}]},
    ]}
    cat, warnings = mods.validate_catalog(raw, remote=True)
    assert [e["id"] for e in cat["mods"]] == ["ok"]
    assert len(warnings) == 6
    with pytest.raises(mods.Error):
        mods.validate_catalog({"mods": "no"}, remote=True)


def test_newer_catalog_from_the_web(user, tmp_path, server):
    with open(mods.CATALOG, encoding="utf-8") as f:
        bundled = json.load(f)
    newer = dict(bundled, updated="2999-01-01", mods=bundled["mods"][:1] + [
        {"id": "extra", "kind": "mod", "name": "Extra", "page_url": "https://example.org/extra"}])
    older = dict(bundled, updated="2000-01-01", mods=bundled["mods"][:1])
    s = server({"/new.json": json.dumps(newer).encode(), "/old.json": json.dumps(older).encode()})
    out = run(user, "catalog", "--refresh", "--catalog-url", s.base + "/new.json")
    assert out["catalog"]["source"] == "remote"
    assert [e["id"] for e in out["entries"]][-1] == "extra"
    # kept for next time, without fetching again
    out = run(user, "catalog")
    assert out["catalog"]["source"] == "cache" and out["entries"][-1]["id"] == "extra"
    # an older one doesn't replace the tools' own
    out = run(user, "catalog", "--refresh", "--catalog-url", s.base + "/old.json")
    assert out["catalog"]["source"] == "bundled"
    # nothing there: the copy here, with a warning
    out = run(user, "catalog", "--refresh", "--catalog-url", s.base + "/missing.json")
    assert out["catalog"]["source"] in ("bundled", "cache")
    assert any("can't be had" in w for w in out["warnings"])


def test_a_mod_download_without_sha256_is_refused():
    """A mod can carry a code patch (native code once recompiled): the catalog has to pin it."""
    base = {"name": "X", "page_url": "https://example.org/x"}
    raw = {"mods": [
        dict(base, id="pinned", kind="mod", downloads=[{"url": "https://example.org/a.ips", "file": "a.ips",
                                                         "archive": "file", "as": "code.ips", "sha256": "0" * 64}]),
        dict(base, id="unpinned", kind="mod", downloads=[{"url": "https://example.org/b.ips", "file": "b.ips",
                                                           "archive": "file", "as": "code.ips"}]),
        dict(base, id="pack", kind="textures", downloads=[{"url": "https://example.org/t.zip"}])]}
    cat, warnings = mods.validate_catalog(raw, remote=True)
    assert [e["id"] for e in cat["mods"]] == ["pinned", "pack"]
    assert len(warnings) == 1 and "unpinned" in warnings[0] and "sha256" in warnings[0]


def test_the_default_catalog_url_is_on_main():
    assert "/MH4UltimateDC/main/tools/mods_catalog.json" in mods.CATALOG_URL
    with open(os.path.join(HERE, "..", "port", "app", "config.h"), encoding="utf-8") as f:
        assert mods.CATALOG_URL in f.read()


# ── texture packs ──

def hd_like(tmp_path):
    """Like the HD pack: a .7z of folders, then a roll-up that replaces UI/ and adds pack.json."""
    base = make_7z(tmp_path / "srv" / "base.7z", [
        ("Armor/male/tex1_8x8_AAAA_13.png", b"armor"), ("Maps/Dunes/tex1_8x8_BBBB_13.png", b"map"),
        ("UI/old/tex1_8x8_CCCC_13.png", b"old ui"), ("UI/tex1_8x8_DDDD_13.png", b"ui v3.0"), ("Maps", None)],
        encode_header=True)
    rollup = make_7z(tmp_path / "srv" / "rollup.7z", [
        ("pack.json", b'{"options": {"skip_mipmap": false, "flip_png_files": true, "use_new_hash": true}}'),
        ("UI/tex1_8x8_DDDD_13.png", b"ui v3.03"), ("Maps/Dunes/tex1_8x8_BBBB_13.png", b"map v3.03")])
    return base, rollup


def test_texture_pack_in_steps(user, tmp_path):
    base, rollup = hd_like(tmp_path)
    cat = catalog(tmp_path / "cat.json", [entry("hd", "textures", [
        {"url": url(base), "size": base.stat().st_size, "archive": "7z"},
        {"url": url(rollup), "archive": "7z", "delete_first": ["ui"]}],
        needs=["custom_textures"], recommended=True)])
    write(user / "load" / "textures" / ".mh4u-staging-0ld" / "big.png")     # an install stopped half-way
    out, events = call(user, "install-from-catalog", "hd", "--catalog", cat)
    assert out["files"] == 4 and out["pack"] == "hd"
    assert "custom textures" in out["messages"][0]
    assert tree(textures(user)) == ["Armor/male/tex1_8x8_AAAA_13.png", "Maps/Dunes/tex1_8x8_BBBB_13.png",
                                    "UI/tex1_8x8_DDDD_13.png", "pack.json"]
    assert (textures(user) / "UI" / "tex1_8x8_DDDD_13.png").read_bytes() == b"ui v3.03"
    assert (textures(user) / "Maps" / "Dunes" / "tex1_8x8_BBBB_13.png").read_bytes() == b"map v3.03"
    kinds = {e["event"] for e in events}
    assert {"step", "download", "unpack", "install"} <= kinds
    assert not os.listdir(user / "mh4u-mods" / ".downloads")           # the downloads are gone
    assert not [p for p in os.listdir(user / "load" / "textures") if p.startswith(".")]   # and the staging
    listed = run(user, "catalog", "--catalog", cat)["entries"][0]
    assert listed["installed"]["as"] == "textures" and listed["installed"]["files"] == 4
    # files of the player's own stay when it is removed
    write(textures(user) / "Mine" / "tex1_1x1_EEEE_13.png", b"mine")
    out = run(user, "remove", "hd")
    assert "removed from the texture folder (4 files)" in out["messages"][0]
    assert tree(textures(user)) == ["Mine/tex1_1x1_EEEE_13.png"]
    assert run(user, "catalog", "--catalog", cat)["entries"][0]["installed"] is None


def test_texture_pack_root_into_and_wrappers(user, tmp_path):
    z = make_zip(tmp_path / "MH4HDUI.zip", {"MH4HDUI-main/README.md": "hi", "MH4HDUI-main/MH4u/UI/a.png": "a",
                                            "MH4HDUI-main/MH4u/UI/sub/b.png": "b",
                                            "MH4HDUI-main/MH4u/Other/c.png": "not this"})
    other = make_zip(tmp_path / "other.zip", {"0004000000126300/pack.json": "{}", "0004000000126300/x/d.png": "d",
                                              "readme.txt": "hi"})
    cat = catalog(tmp_path / "cat.json", [
        entry("ui", "textures", [{"url": url(z), "archive": "zip", "root": "mh4u/ui", "into": "UI"}],
              conflicts=["other"]),
        entry("other", "textures", [{"url": url(other)}], conflicts=["ui"])])
    run(user, "install-from-catalog", "ui", "--catalog", cat)
    assert tree(textures(user)) == ["UI/a.png", "UI/sub/b.png"]
    err = run(user, "install-from-catalog", "other", "--catalog", cat, code=1)
    assert err["error"]["code"] == "conflict" and err["error"]["entries"] == ["ui"]
    run(user, "install-from-catalog", "other", "--catalog", cat, "--force")
    assert tree(textures(user)) == ["UI/a.png", "UI/sub/b.png", "pack.json", "x/d.png"]   # the title folder looked through
    run(user, "remove", "ui")
    assert tree(textures(user)) == ["pack.json", "x/d.png"]


def test_texture_pack_files_of_another_pack_or_the_players(user, tmp_path):
    one = make_zip(tmp_path / "one.zip", {"same.png": "1", "a.png": "a"})
    two = make_zip(tmp_path / "two.zip", {"same.png": "2"})
    run(user, "install", "--textures", str(one), "--name", "One")
    err = run(user, "install", "--textures", str(two), "--name", "Two", code=1)
    assert err["error"]["code"] == "conflict" and err["error"]["packs"] == ["One"]
    out = run(user, "install", "--textures", str(two), "--name", "Two", "--force")
    assert (textures(user) / "same.png").read_text() == "2"
    run(user, "remove", "One")                  # same.png is Two's now: it stays
    assert tree(textures(user)) == ["same.png"]
    write(textures(user) / "mine.png", "mine")
    three = make_zip(tmp_path / "three.zip", {"mine.png": "3"})
    err = run(user, "install", "--textures", str(three), code=1)
    assert err["error"]["code"] == "foreign_files" and err["error"]["files"] == ["mine.png"]
    assert (textures(user) / "mine.png").read_text() == "mine"
    out = run(user, "install", "--textures", str(three), "--force")
    assert out["replaced"] == 1
    assert run(user, "install", "--textures", str(three), code=1)["error"]["code"] == "exists"
    run(user, "install", "--textures", str(three), "--replace")


def test_texture_pack_for_another_game(user, tmp_path):
    other = make_zip(tmp_path / "other.zip", {"0004000000999900/a.png": "a"})
    err = run(user, "install", "--textures", str(other), code=1)
    assert err["error"]["code"] == "other_title"
    assert not textures(user).exists() or tree(textures(user)) == []


def test_texture_pack_for_the_other_region(user, tmp_path):
    """MH4U's textures are the same assets in both regions (Azahar matches them by content), so a
    pack made for Europe goes into the USA game's folder, and the reverse, with a note."""
    eu = make_zip(tmp_path / "eu.zip", {"0004000000126100/a.png": "a"})
    out = run(user, "install", "--textures", str(eu))
    assert tree(textures(user)) == ["a.png"]
    assert out["messages"][1] == ("a texture pack made for MH4U Europe (0004000000126100/), used for MH4U USA: Azahar "
                                  "finds textures by their content, and the two games' are the same but for some with "
                                  "text")
    us = make_zip(tmp_path / "us.zip", {"0004000000126300/b.png": "b"})
    out = run(user, "--program-id", "0004000000126100", "install", "--textures", str(us), "--name", "us")
    assert tree(user / "load" / "textures" / "0004000000126100") == ["b.png"]
    assert out["messages"][1].startswith("a texture pack made for MH4U USA (0004000000126300/), used for MH4U Europe")
    # a pack for the game's own region says nothing more
    out = run(user, "install", "--textures", str(make_zip(tmp_path / "own.zip", {TID + "/c.png": "c"})))
    assert len(out["messages"]) == 1


EUR = "0004000000126100"


def eur_textures(user):
    return user / "load" / "textures" / EUR


def test_a_catalog_mod_is_for_the_game_of_its_region(user, tmp_path):
    """A catalog mod's code patch is made for the code of the region its entry names: it is refused
    for the other region's game (--force installs it anyway), and once installed for its own,
    it keeps that region and is left out of the other game's mods folder. The refusal says what is
    known: a patch that changes that code breaks the other game, and one that changes only data
    (laid out the same in both) hasn't been tried with it."""
    code = write(tmp_path / "srv" / "code.ips", ips_patch(0x100, b"\1\2"))
    cat = catalog(tmp_path / "cat.json", [entry("usa-mod", "mod", [
        {"url": url(code), "sha256": hashlib.sha256(code.read_bytes()).hexdigest(), "archive": "file",
         "as": "exefs/code.ips"}], region="usa")])                      # (any capitals: USA)
    eu = ("--program-id", EUR)
    listed = run(user, *eu, "catalog", "--catalog", cat)["entries"][0]
    assert listed["region"] == "USA" and listed["fits"] is False
    assert run(user, "catalog", "--catalog", cat)["entries"][0]["fits"] is True
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        assert mods.main(["--user-dir", str(user), *eu, "catalog", "--catalog", cat]) == 0
    assert "made for MH4U USA's code, not for MH4U Europe" in buf.getvalue()
    err = run(user, *eu, "install-from-catalog", "usa-mod", "--catalog", cat, code=1)["error"]
    assert err["code"] == "other_region" and err["region"] == "USA" and err["game"] == "Europe"
    assert err["message"] == ("Usa Mod is made for MH4U USA's code, and the game is MH4U Europe (0004000000126100): a "
                              "patch that changes that code breaks this game, and one that changes only data hasn't "
                              "been tried with it (--force installs it anyway)")
    assert run(user, "list")["mods"] == [] and not (user / "mh4u-mods" / ".downloads").exists()
    # for the USA game: installed, with its region
    out = run(user, "install-from-catalog", "usa-mod", "--catalog", cat)
    assert json.loads((user / "mh4u-mods" / out["mod"] / "mod.json").read_text())["region"] == "USA"
    run(user, "deploy")
    assert (user / "load" / "mods" / TID / "exefs" / "code.ips").is_file()
    mod = run(user, *eu, "list")["mods"][0]
    assert mod["enabled"] and mod["fits"] is False and "left out of MH4U Europe's mods folder" in mod["warnings"][0]
    out = run(user, *eu, "deploy")
    assert out["added"] == [] and any("left out" in w for w in out["warnings"])
    assert not (user / "load" / "mods" / EUR / "exefs").exists()
    # --force for the European game: its region is "any", so it is put in place for both games
    # (with the warning every code patch without a title folder gets for the European one)
    run(user, "remove", "usa-mod")
    run(user, "deploy")
    out = run(user, *eu, "install-from-catalog", "usa-mod", "--catalog", cat, "--force")
    assert any("installed anyway (--force)" in m for m in out["messages"])
    assert json.loads((user / "mh4u-mods" / out["mod"] / "mod.json").read_text())["region"] == "any"
    mod = next(m for m in out["mods"] if m["id"] == out["mod"])
    assert mod["fits"] is True and "if this code patch was made for MH4U USA's code" in mod["warnings"][0]
    assert out["warnings"] == []                    # (in the mod's own warnings, not again in the result's)
    assert run(user, *eu, "deploy")["added"] == ["exefs/code.ips"]
    assert run(user, "deploy")["added"] == ["exefs/code.ips"]
    # an entry without a region is for both games too
    cat = catalog(tmp_path / "cat.json", [entry("any-mod", "mod", [
        {"url": url(code), "sha256": hashlib.sha256(code.read_bytes()).hexdigest(), "archive": "file",
         "as": "exefs/code.ips"}])])
    out = run(user, "install-from-catalog", "any-mod", "--catalog", cat)
    assert json.loads((user / "mh4u-mods" / out["mod"] / "mod.json").read_text())["region"] == "any"


def test_a_catalog_mod_installed_without_a_region_is_the_usa_games(user, tmp_path):
    """A catalog mod installed before mod.json kept the entry's region (0.1.6 and older) has none,
    and every mod in the catalog was made for the USA game then: it counts as that game's, and is
    left out of the European game's mods folder. Installed again with --force for the European
    game, it says "any", and goes in both games' folders."""
    lib = user / "mh4u-mods"
    old = lib / "100% crafting success"                     # as 0.1.6's install-from-catalog left it
    write(old / "mod.json", json.dumps({"name": "100% crafting success", "version": "0.0.2", "author": "anco",
                                        "description": "Every item combination succeeds.",
                                        "url": "https://codeberg.org/anco/mh4u_qol", "catalog": "anco-crafting-100"}))
    write(old / "exefs" / "code.ips", ips_patch(0x100, b"\1"))
    write(lib / "library.json", json.dumps({"format": 1, "mods": [{"id": "100% crafting success", "enabled": True}]}))
    eu = ("--program-id", EUR)
    mod = run(user, *eu, "list")["mods"][0]
    assert mod["enabled"] and mod["region"] == "" and mod["fits"] is False and mod["errors"] == []
    assert mod["warnings"] == ["made for MH4U USA (installed from the catalog by an older version of the mod manager, "
                               "when all the catalog's mods were made for that game): its code patch is at that code's "
                               "addresses, so it is left out of MH4U Europe's mods folder"]
    out = run(user, *eu, "deploy")
    assert out["added"] == [] and out["warnings"] == ["100% crafting success: " + mod["warnings"][0]]
    assert not (user / "load" / "mods" / EUR / "exefs").exists()
    # for the USA game, as before
    mod = run(user, "list")["mods"][0]
    assert mod["fits"] is True and mod["warnings"] == []
    assert run(user, "deploy")["added"] == ["exefs/code.ips"]
    # installed again with --force for the European game: for both
    code = write(tmp_path / "srv" / "code.ips", ips_patch(0x100, b"\1\2"))
    cat = catalog(tmp_path / "cat.json", [entry("anco-crafting-100", "mod", [
        {"url": url(code), "sha256": hashlib.sha256(code.read_bytes()).hexdigest(), "archive": "file",
         "as": "exefs/code.ips"}], name="100% crafting success", region="USA")])
    out = run(user, *eu, "install-from-catalog", "anco-crafting-100", "--catalog", cat, "--force")
    assert out["mod"] == "100% crafting success"                    # in place of the old one
    assert json.loads((old / "mod.json").read_text())["region"] == "any"
    assert run(user, *eu, "deploy")["added"] == ["exefs/code.ips"]
    assert run(user, "deploy")["updated"] == ["exefs/code.ips"]
    for game in (EUR, TID):
        assert (user / "load" / "mods" / game / "exefs" / "code.ips").read_bytes() == code.read_bytes(), game


def test_a_catalog_texture_pack_for_the_european_game(user, tmp_path):
    """A texture pack is for either region's game: the USA build goes into the European game's
    texture folder, and the install says so."""
    pack = make_zip(tmp_path / "srv" / "pack.zip", {TID + "/pack.json": "{}", TID + "/UI/a.png": "a"})
    cat = catalog(tmp_path / "cat.json", [entry("pack", "textures", [{"url": url(pack)}], region="USA",
                                                needs=["custom_textures"])])
    eu = ("--program-id", EUR)
    assert run(user, *eu, "catalog", "--catalog", cat)["entries"][0]["fits"] is True
    out = run(user, *eu, "install-from-catalog", "pack", "--catalog", cat)
    assert tree(eur_textures(user)) == ["UI/a.png", "pack.json"] and not textures(user).exists()
    assert out["textures_folder"] == str(eur_textures(user))
    assert out["messages"][1].startswith(f"a texture pack made for MH4U USA ({TID}/), used for MH4U Europe")


def test_each_game_has_a_texture_record_of_its_own(user, tmp_path):
    """A pack installed for one game is in that game's folder and record only: the other game's
    catalog doesn't count it as installed, and its remove doesn't touch it."""
    pack = make_zip(tmp_path / "srv" / "pack.zip", {"pack.json": "{}", "UI/a.png": "a"})
    cat = catalog(tmp_path / "cat.json", [entry("pack", "textures", [{"url": url(pack)}], region="USA")])
    lib = user / "mh4u-mods"
    run(user, "install-from-catalog", "pack", "--catalog", cat)                 # for the USA game
    usa_record = (lib / "textures.json").read_bytes()
    eu = ("--program-id", EUR)
    out = run(user, *eu, "catalog", "--catalog", cat)
    assert out["entries"][0]["installed"] is None and out["textures"] == []
    assert out["textures_folder"] == str(eur_textures(user))
    assert run(user, *eu, "remove", "pack", code=1)["error"]["code"] == "not_found"
    assert tree(textures(user)) == ["UI/a.png", "pack.json"] and (lib / "textures.json").read_bytes() == usa_record
    assert not (lib / f"textures-{EUR}.json").exists()
    # installed for the European game too: in a record of its own
    run(user, *eu, "install-from-catalog", "pack", "--catalog", cat)
    assert tree(eur_textures(user)) == ["UI/a.png", "pack.json"]
    assert json.loads((lib / f"textures-{EUR}.json").read_text())["packs"]["pack"]["files"] == ["UI/a.png",
                                                                                                "pack.json"]
    assert (lib / "textures.json").read_bytes() == usa_record
    # removed there, it stays for the USA game
    run(user, *eu, "remove", "pack")
    assert tree(eur_textures(user)) == [] and tree(textures(user)) == ["UI/a.png", "pack.json"]
    assert run(user, "catalog", "--catalog", cat)["entries"][0]["installed"]["as"] == "textures"
    assert run(user, *eu, "catalog", "--catalog", cat)["entries"][0]["installed"] is None
    run(user, "remove", "pack")
    assert tree(textures(user)) == []


def test_a_european_pack_in_the_usa_record_moves_to_its_own(user, tmp_path):
    """For a while the European game's texture packs were recorded in textures.json with USA's.
    The first time either game's record is read while the European game has none, a pack there
    whose files are all in Europe's folder and none in USA's moves to it; any other stays where
    it is, and no file is touched."""
    lib = user / "mh4u-mods"
    for rel in ("UI/a.png", "pack.json", "both.png"):
        write(eur_textures(user) / rel, "eu")
    for rel in ("usa.png", "both.png"):
        write(textures(user) / rel, "usa")
    write(lib / "textures.json", json.dumps({"format": 1, "packs": {
        "eu-pack": {"name": "EU pack", "catalog": "pack", "files": ["UI/a.png", "pack.json"]},
        "usa-pack": {"name": "USA pack", "files": ["usa.png"]},
        "both": {"name": "Both", "files": ["both.png"]},
        "empty": {"name": "Empty", "files": []},
        "outside": {"name": "Outside", "files": ["../usa.png"]}}}))
    out = run(user, "--program-id", EUR, "catalog", "--catalog-url", "")
    assert [t["id"] for t in out["textures"]] == ["eu-pack"]
    assert sorted(json.loads((lib / f"textures-{EUR}.json").read_text())["packs"]) == ["eu-pack"]
    assert sorted(json.loads((lib / "textures.json").read_text())["packs"]) == ["both", "empty", "outside", "usa-pack"]
    assert tree(eur_textures(user)) == ["UI/a.png", "both.png", "pack.json"]
    assert tree(textures(user)) == ["both.png", "usa.png"]
    # once the European game has a record, nothing moves again
    write(eur_textures(user) / "usa.png", "eu")
    os.remove(textures(user) / "usa.png")
    run(user, "--program-id", EUR, "catalog", "--catalog-url", "")
    assert "usa-pack" in json.loads((lib / "textures.json").read_text())["packs"]
    # a USA read first moves it too (a pack installed for the USA game has its files in USA's
    # folder): the USA game doesn't count it as installed, and its remove doesn't forget the files
    os.remove(lib / f"textures-{EUR}.json")
    write(lib / "textures.json", json.dumps({"format": 1, "packs": {
        "eu-pack": {"name": "EU pack", "catalog": "pack", "files": ["UI/a.png", "pack.json"]},
        "both": {"name": "Both", "files": ["both.png"]}}}))
    assert [t["id"] for t in run(user, "catalog", "--catalog-url", "")["textures"]] == ["both"]
    assert sorted(json.loads((lib / f"textures-{EUR}.json").read_text())["packs"]) == ["eu-pack"]
    assert run(user, "remove", "eu-pack", code=1)["error"]["code"] == "not_found"
    assert tree(eur_textures(user)) == ["UI/a.png", "both.png", "pack.json", "usa.png"]
    assert [t["id"] for t in run(user, "--program-id", EUR, "catalog", "--catalog-url", "")["textures"]] == ["eu-pack"]
    run(user, "--program-id", EUR, "remove", "eu-pack")
    assert tree(eur_textures(user)) == ["both.png", "usa.png"] and tree(textures(user)) == ["both.png"]
    assert sorted(json.loads((lib / "textures.json").read_text())["packs"]) == ["both"]


def test_texture_pack_names_one_file_on_windows(user, tmp_path):
    # a texture pack shouldn't fail for one such pair: the later one is used, with a warning
    pack = make_7z(tmp_path / "p.7z", [("UI/A.png", b"1"), ("ui/a.png", b"2"), ("b.png", b"b")])
    out = run(user, "install", "--textures", str(pack))
    assert any("one file on Windows" in w for w in out["warnings"])
    assert len(tree(textures(user))) == 2


# ── mods ──

def ips_patch(offset, data):
    return b"PATCH" + offset.to_bytes(3, "big") + len(data).to_bytes(2, "big") + data + b"EOF"


def test_mod_single_file_and_zip_layouts(user, tmp_path):
    code = write(tmp_path / "srv" / "code.ips", ips_patch(0xE2BCDE, b"\x64"))     # in the data: data only
    digest = hashlib.sha256(code.read_bytes()).hexdigest()
    life = make_zip(tmp_path / "srv" / "life.zip", {
        "Pkg/Azahar/load/mods/0004000000126300/exefs/code.ips": ips_patch(0x100, b"\0\0\0\0"),
        "Pkg/Luma3DS/luma/titles/0004000000126300/code.ips": ips_patch(0x100, b"\0\0\0\0"),
        "Pkg/README_en.md": "readme"})
    romfs = make_7z(tmp_path / "srv" / "romfs.7z", [("Some mod/romfs/data/x.arc", b"arc"),
                                                     ("Some mod/readme.txt", b"hi")])
    cat = catalog(tmp_path / "cat.json", [
        entry("crafting", "mod", [{"url": url(code), "size": code.stat().st_size, "sha256": digest, "archive": "file",
                                   "as": "exefs/code.ips"}]),
        entry("life", "mod", [{"url": url(life), "archive": "zip", "root": "Azahar"}]),
        entry("romfs", "mod", [{"url": url(romfs)}]),
        entry("page", "mod", [])])
    out = run(user, "install-from-catalog", "crafting", "--catalog", cat)
    mod = next(m for m in out["mods"] if m["id"] == out["mod"])
    assert mod["catalog"] == "crafting" and mod["name"] == "Crafting" and mod["enabled"]
    assert mod["code"]["changes_code"] is False and mod["code"]["adds_code"] is False
    lib = user / "mh4u-mods" / out["mod"]
    assert (lib / "exefs" / "code.ips").read_bytes() == code.read_bytes()
    assert json.loads((lib / "mod.json").read_text())["catalog"] == "crafting"
    out = run(user, "install-from-catalog", "life", "--catalog", cat)
    life_mod = next(m for m in out["mods"] if m["id"] == out["mod"])
    assert not life_mod["enabled"] and any("stays off" in m for m in out["messages"])   # one code mod at a time
    assert life_mod["layout"] == "title"
    out = run(user, "install-from-catalog", "romfs", "--catalog", cat)
    assert next(m for m in out["mods"] if m["id"] == out["mod"])["contains"] == ["romfs"]
    # installing again updates it in place
    out = run(user, "install-from-catalog", "crafting", "--catalog", cat)
    assert [m["id"] for m in out["mods"]].count("Crafting") == 1
    listed = {e["id"]: e for e in run(user, "catalog", "--catalog", cat)["entries"]}
    assert listed["crafting"]["installed"]["as"] == "mod" and listed["crafting"]["installed"]["enabled"]
    assert listed["page"]["direct"] is False
    assert run(user, "install-from-catalog", "page", "--catalog", cat, code=1)["error"]["code"] == "page_only"
    out = run(user, "remove", "crafting")                       # by its catalog id
    assert "Crafting" not in [m["id"] for m in out["mods"]]


def test_checksum_and_bad_archives_install_nothing(user, tmp_path):
    good = make_zip(tmp_path / "m.zip", {"romfs/data/x.arc": "x"})
    evil = make_zip(tmp_path / "evil.zip", {"../../outside.txt": "x", "romfs/data/y.arc": "y"})
    evil7 = make_7z(tmp_path / "evil.7z", [("romfs/data/a.arc", b"a"), ("../b.arc", b"b")])
    rar = write(tmp_path / "pack.rar", b"Rar!\x1a\x07\x01\x00 rest")
    cat = catalog(tmp_path / "cat.json", [
        entry("sum", "mod", [{"url": url(good), "sha256": "0" * 64}]),
        entry("evil", "mod", [{"url": url(evil)}]),
        entry("evil7", "textures", [{"url": url(evil7)}]),
        entry("rar", "textures", [{"url": url(rar)}])])
    assert run(user, "install-from-catalog", "sum", "--catalog", cat, code=1)["error"]["code"] == "checksum"
    assert run(user, "install-from-catalog", "evil", "--catalog", cat, code=1)["error"]["code"] == "archive"
    assert run(user, "install-from-catalog", "evil7", "--catalog", cat, code=1)["error"]["code"] == "archive"
    err = run(user, "install-from-catalog", "rar", "--catalog", cat, code=1)["error"]
    assert err["code"] == "manual_extract" and err["target"] == str(textures(user))
    assert os.path.isfile(err["download"])                     # kept, to unpack by hand
    assert not (tmp_path / "outside.txt").exists() and not (user / "outside.txt").exists()
    assert run(user, "list")["mods"] == []
    assert not textures(user).exists() or tree(textures(user)) == []
    assert not [p for p in os.listdir(user / "mh4u-mods") if p.startswith(".adding")]


def test_download_resumes_and_checks(user, tmp_path, server):
    data = os.urandom(3 << 20)
    blob = make_zip(tmp_path / "big.zip", {"romfs/data/big.arc": data})
    raw = blob.read_bytes()
    s = server({"/big.zip": raw})
    downloads = user / "mh4u-mods" / ".downloads"
    write(downloads / "big.zip.part", raw[:1000])               # a download that stopped
    cat = catalog(tmp_path / "cat.json", [entry("big", "mod", [
        {"url": s.base + "/big.zip", "file": "big.zip", "size": len(raw),
         "sha256": hashlib.sha256(raw).hexdigest()}])])
    out, events = call(user, "install-from-catalog", "big", "--catalog", cat, "--keep-download")
    assert s.requests == [("/big.zip", "bytes=1000-")]
    assert (downloads / "big.zip").read_bytes() == raw          # kept, as asked
    started = next(e for e in events if e["event"] == "download")
    assert started["resumed"] == 1000 and started["total"] == len(raw)
    assert (user / "mh4u-mods" / out["mod"] / "romfs" / "data" / "big.arc").read_bytes() == data
    # a server without ranges: from the start again
    s2 = server({"/big.zip": raw}, ranges=False)
    (downloads / "big.zip").unlink()
    write(downloads / "big.zip.part", raw[:1000])
    path = mods.download(s2.base + "/big.zip", str(downloads), "big.zip", len(raw))
    assert open(path, "rb").read() == raw
    # a missing file
    with pytest.raises(mods.Error) as e:
        mods.download(s2.base + "/nothing.zip", str(downloads), "nothing.zip")
    assert e.value.code == "download" and "404" in e.value.message


def test_install_from_url_and_archives(user, tmp_path, server):
    z7 = make_7z(tmp_path / "mod.7z", [("My Mod/romfs/data/a.arc", b"a")], method="bcj+lzma")
    s = server({"/files/My%20Mod.7z": z7.read_bytes()})
    out = run(user, "install", s.base + "/files/My%20Mod.7z")
    assert out["mod"] == "My Mod" and out["mods"][0]["contains"] == ["romfs"]
    assert not os.listdir(user / "mh4u-mods" / ".downloads")
    # add takes a .7z and a .tar.gz too now
    tar = tmp_path / "t.tar.gz"
    with tarfile.open(tar, "w:gz") as t:
        info = tarfile.TarInfo("romfs/data/b.arc")
        info.size = 1
        t.addfile(info, io.BytesIO(b"b"))
    out = run(user, "add", str(tar), "--name", "Tarred")
    assert out["mod"] == "Tarred"
    assert (user / "mh4u-mods" / "Tarred" / "romfs" / "data" / "b.arc").read_bytes() == b"b"


def test_code_mod_with_new_code_needs_a_library_built_with_it(user, tmp_path, monkeypatch):
    """A code mod that adds code after the game's own (a "code cave") runs on the port once the
    code library is built with it, which then recompiles the code's last page to its end
    (the build record's code_size); a library built without it, or by an older build_game.py
    that left the cave out, has to be built again."""
    text, end = mods.text_size(), mods.code_segment_end(mods.text_size())
    if text is None or end is None:
        pytest.skip("no config/versions.json")
    patch = ips_patch(text + 0x10, b"\x01\x02\x03\x04")
    cave = make_zip(tmp_path / "cave.zip", {"exefs/code.ips": patch})
    out = run(user, "add", str(cave), "--name", "Cave")
    assert out["mods"][0]["code"]["adds_code"] is True
    assert out["mods"][0]["code"]["changes_code"] is True          # the cave is part of the checked code
    run(user, "deploy")
    lib = write(user / "recomp" / (TID + (".dll" if sys.platform.startswith("win") else
                                         ".dylib" if sys.platform == "darwin" else ".so")), b"lib")
    status = run(user, "status")["code_library"]
    assert status["state"] == "unknown" and status["adds_code"] is True     # no record
    mods.write_build_record(str(lib), [], text, False)                      # built without the mod
    assert run(user, "status")["code_library"]["state"] == "rebuild"
    deployed = user / "load" / "mods" / TID / "exefs" / "code.ips"
    mods.write_build_record(str(lib), [str(deployed)], text, True)          # an older build_game.py
    status = run(user, "status")["code_library"]
    assert status["state"] == "rebuild" and "older" in status["reason"]
    mods.write_build_record(str(lib), [str(deployed)], text, True, code_size=end)
    status = run(user, "status")["code_library"]
    assert status["state"] == "ok" and status["adds_code"] is True
    assert lib.exists()
