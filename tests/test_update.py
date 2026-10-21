"""tools/update.py: the launcher's Update button. Checks GitHub's latest release against the app's
commit, downloads the package for the platform, checks its SHA-256 and puts its files over the
installed ones (a file in use renamed out of the way). Run against a small local server that
answers like GitHub's API, with synthetic packages (never a real release)."""
from __future__ import annotations

import hashlib
import http.server
import io
import json
import os
import stat
import subprocess
import sys
import tarfile
import threading
import zipfile

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")
sys.path.insert(0, TOOLS)

import update  # noqa: E402


def windows_zip(files: dict[str, bytes], top="mh4u-port-windows") -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        for name, data in files.items():
            z.writestr(f"{top}/{name}", data)
    return buf.getvalue()


def linux_tar(files: dict[str, bytes], top="mh4u-port-linux", links=None) -> bytes:
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as t:
        for name, data in files.items():
            info = tarfile.TarInfo(f"{top}/{name}")
            info.size = len(data)
            info.mode = 0o755 if name.endswith((".AppImage", ".sh")) else 0o644
            t.addfile(info, io.BytesIO(data))
        for name, target in (links or {}).items():
            info = tarfile.TarInfo(f"{top}/{name}")
            info.type = tarfile.SYMTYPE
            info.linkname = target
            t.addfile(info)
    return buf.getvalue()


class FakeGitHub:
    """GET /repos/R/releases/latest, /releases/tags/T, /compare/A...B and the assets."""

    def __init__(self):
        self.releases: list[dict] = []
        self.assets: dict[str, bytes] = {}
        self.compare: dict[str, str] = {}  # "A...B" -> status
        handler = self._handler()
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        self.url = f"http://127.0.0.1:{self.server.server_address[1]}"
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def add_release(self, tag, assets: dict[str, bytes], digest=True, digest_override=None):
        rel = {"tag_name": tag, "name": f"MH4U PC port {tag[1:]}", "html_url": f"https://example/{tag}",
               "published_at": "2026-10-20T00:00:00Z", "body": "notes", "prerelease": False, "assets": []}
        for name, data in assets.items():
            self.assets[f"{tag}/{name}"] = data
            asset = {"name": name, "size": len(data), "browser_download_url": f"{self.url}/download/{tag}/{name}"}
            if digest:
                asset["digest"] = "sha256:" + (digest_override or hashlib.sha256(data).hexdigest())
            rel["assets"].append(asset)
        self.releases.append(rel)

    def _handler(self):
        gh = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def _send(self, code, body: bytes, kind="application/json"):
                self.send_response(code)
                self.send_header("Content-Type", kind)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                path = self.path
                prefix = f"/repos/{update.REPO}/"
                if path.startswith("/download/"):
                    data = gh.assets.get(path[len("/download/"):])
                    return self._send(200, data, "application/octet-stream") if data else self._send(404, b"")
                if not path.startswith(prefix):
                    return self._send(404, b"{}")
                rest = path[len(prefix):]
                if rest == "releases/latest" and gh.releases:
                    return self._send(200, json.dumps(gh.releases[-1]).encode())
                if rest.startswith("releases/tags/"):
                    for rel in gh.releases:
                        if rel["tag_name"] == rest[len("releases/tags/"):]:
                            return self._send(200, json.dumps(rel).encode())
                if rest.startswith("compare/") and rest[len("compare/"):] in gh.compare:
                    return self._send(200, json.dumps({"status": gh.compare[rest[len("compare/"):]]}).encode())
                return self._send(404, b'{"message": "Not Found"}')

        return Handler


@pytest.fixture
def github(monkeypatch):
    gh = FakeGitHub()
    monkeypatch.setenv("MH4U_UPDATE_API", gh.url)
    monkeypatch.delenv("MH4U_UPDATE_REPO", raising=False)
    yield gh
    gh.server.shutdown()


def run(*args, code=0):
    """update.py as the launcher runs it: its progress lines, then one JSON object."""
    r = subprocess.run([sys.executable, "-u", os.path.join(TOOLS, "update.py"), "--json", *args],
                       capture_output=True, text=True, timeout=120, env=os.environ.copy())
    assert r.returncode == code, (r.stdout, r.stderr)
    lines = r.stdout.splitlines()
    progress = [json.loads(line[1:]) for line in lines if line.startswith("@")]
    out = json.loads([line for line in lines if not line.startswith("@")][-1])
    assert out["ok"] is (code == 0)
    return out, progress


def installed(tmp_path, files: dict[str, bytes]):
    root = tmp_path / "mh4u-port-windows"
    for name, data in files.items():
        (root / name).parent.mkdir(parents=True, exist_ok=True)
        (root / name).write_bytes(data)
    return root


@pytest.mark.parametrize("status,expected", [("ahead", "update"), ("identical", "current"),
                                             ("behind", "newer"), ("diverged", "update")])
def test_check_compares_commits(github, status, expected):
    github.add_release("v0.1.3", {"mh4u-port-windows.zip": windows_zip({"mh4u.exe": b"new"})})
    github.compare["abc1234...v0.1.3"] = status
    out, _ = run("check", "--platform", "windows", "--commit", "abc1234")
    assert out["status"] == expected
    assert out["tag"] == "v0.1.3" and out["name"] == "MH4U PC port 0.1.3"
    assert out["url"] == "https://example/v0.1.3" and out["size"] > 0


def test_check_unknown_commit(github):
    github.add_release("v0.1.3", {"mh4u-port-linux.tar.gz": linux_tar({"mh4u.AppImage": b"x"})})
    out, _ = run("check", "--platform", "linux", "--commit", "deadbee")
    assert out["status"] == "unknown"
    out, _ = run("check", "--platform", "linux")
    assert out["status"] == "unknown"


def test_check_without_a_release(github):
    out, _ = run("check", "--platform", "windows", "--commit", "abc", code=1)
    assert "no release" in out["error"]["message"]


def test_check_without_the_platforms_package(github):
    github.add_release("v0.1.3", {"mh4u-port-linux.tar.gz": linux_tar({"a": b"x"})})
    out, _ = run("check", "--platform", "macos", "--commit", "abc", code=1)
    assert "mh4u-port-macos.zip" in out["error"]["message"]


def test_github_unreachable(monkeypatch):
    monkeypatch.setenv("MH4U_UPDATE_API", "http://127.0.0.1:9")
    out, _ = run("check", "--platform", "windows", code=1)
    assert "couldn't be reached" in out["error"]["message"]


def test_install_windows_package(github, tmp_path):
    root = installed(tmp_path, {"mh4u.exe": b"old", "SDL2.dll": b"old", "mh4u-tools/tools/update.py": b"old",
                                "player-notes.txt": b"mine"})
    github.add_release("v0.1.3", {"mh4u-port-windows.zip": windows_zip(
        {"mh4u.exe": b"new exe", "SDL2.dll": b"new dll", "mh4u-tools/tools/update.py": b"new tool",
         "mh4u-tools/tools/brand_new.py": b"added"})})
    out, progress = run("install", "--platform", "windows", "--to", str(root))
    assert out["tag"] == "v0.1.3" and out["files"] == 4 and out["renamed"] == [] and out["verified"]
    assert out["app"] == os.path.join(str(root), "mh4u.exe")
    assert (root / "mh4u.exe").read_bytes() == b"new exe"
    assert (root / "mh4u-tools/tools/brand_new.py").read_bytes() == b"added"
    assert (root / "player-notes.txt").read_bytes() == b"mine"  # not the package's: left alone
    assert not (root / update.WORK).exists()
    # the launcher's progress window reads these (launcher/tools.cpp, ModsProgress)
    assert any(p.get("message", "").startswith("Downloading") for p in progress)
    assert any(p.get("what") == "Downloading the update" and p.get("total") for p in progress)


def test_install_linux_keeps_modes_and_links(github, tmp_path):
    root = installed(tmp_path, {"mh4u.AppImage": b"old", "mh4u-tools/LICENSE": b"gpl"})
    github.add_release("v0.2.0", {"mh4u-port-linux.tar.gz": linux_tar(
        {"mh4u.AppImage": b"new", "mh4u-tools/run.sh": b"#!/bin/sh"}, links={"current": "mh4u.AppImage"})})
    out, _ = run("install", "--platform", "linux", "--to", str(root))
    assert out["app"].endswith("mh4u.AppImage")
    assert os.stat(root / "mh4u.AppImage").st_mode & stat.S_IXUSR
    assert os.readlink(root / "current") == "mh4u.AppImage"


def test_install_refuses_a_bad_checksum(github, tmp_path):
    root = installed(tmp_path, {"mh4u.exe": b"old", "mh4u-tools/LICENSE": b"gpl"})
    github.add_release("v0.1.3", {"mh4u-port-windows.zip": windows_zip({"mh4u.exe": b"evil"})},
                       digest_override="0" * 64)
    out, _ = run("install", "--platform", "windows", "--to", str(root), code=1)
    assert "checksum" in out["error"]["message"]
    assert (root / "mh4u.exe").read_bytes() == b"old"


def test_install_refuses_files_outside_the_folder(github, tmp_path):
    root = installed(tmp_path, {"mh4u.exe": b"old", "mh4u-tools/LICENSE": b"gpl"})
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr("mh4u-port-windows/../../escaped.txt", b"x")
    github.add_release("v0.1.3", {"mh4u-port-windows.zip": buf.getvalue()})
    out, _ = run("install", "--platform", "windows", "--to", str(root), code=1)
    assert "outside" in out["error"]["message"]
    assert not (tmp_path / "escaped.txt").exists() and not (tmp_path.parent / "escaped.txt").exists()


def test_install_refuses_links_outside_the_folder(github, tmp_path):
    root = installed(tmp_path, {"mh4u.AppImage": b"old", "mh4u-tools/LICENSE": b"gpl"})
    github.add_release("v0.2.0", {"mh4u-port-linux.tar.gz": linux_tar({}, links={"evil": "../../etc/passwd"})})
    out, _ = run("install", "--platform", "linux", "--to", str(root), code=1)
    assert "link outside" in out["error"]["message"]


def test_install_needs_a_package(github, tmp_path):
    github.add_release("v0.1.3", {"mh4u-port-windows.zip": windows_zip({"mh4u.exe": b"new"})})
    out, _ = run("install", "--platform", "windows", "--to", str(tmp_path), code=1)
    assert "isn't a package" in out["error"]["message"]


def test_a_file_in_use_is_renamed_and_cleaned_up_later(tmp_path):
    new = tmp_path / "new"
    root = tmp_path / "root"
    for base, data in ((new, b"new"), (root, b"old")):
        (base / "bin").mkdir(parents=True)
        (base / "mh4u.exe").write_bytes(data)
        (base / "bin" / "SDL2.dll").write_bytes(data)

    def windows_like_replace(src, dst):
        # Windows can't replace a running program or a loaded DLL, but can rename it
        if os.path.exists(dst) and open(dst, "rb").read() == b"old":
            raise PermissionError(dst)
        os.replace(src, dst)

    moved, renamed = update.put_in_place(str(new), str(root), replace=windows_like_replace)
    assert moved == 2
    assert sorted(renamed) == sorted(["mh4u.exe", os.path.join("bin", "SDL2.dll")])
    assert (root / "mh4u.exe").read_bytes() == b"new"
    assert (root / ("mh4u.exe" + update.OLD_SUFFIX)).read_bytes() == b"old"
    removed = update.cleanup(str(root))
    assert sorted(removed) == sorted(["mh4u.exe" + update.OLD_SUFFIX, os.path.join("bin", "SDL2.dll" + update.OLD_SUFFIX)])
    assert sorted(os.listdir(root)) == ["bin", "mh4u.exe"]


def test_a_mac_app_is_swapped_whole(tmp_path):
    new = tmp_path / "new"
    root = tmp_path / "root"
    (new / "MH4U.app/Contents/MacOS").mkdir(parents=True)
    (new / "MH4U.app/Contents/MacOS/mh4u").write_bytes(b"new")
    (root / "MH4U.app/Contents/MacOS").mkdir(parents=True)
    (root / "MH4U.app/Contents/MacOS/mh4u").write_bytes(b"old")
    (root / "MH4U.app/Contents/stale.dylib").write_bytes(b"old")  # gone from the new app
    update.put_in_place(str(new), str(root))
    assert (root / "MH4U.app/Contents/MacOS/mh4u").read_bytes() == b"new"
    assert not (root / "MH4U.app/Contents/stale.dylib").exists()
    update.cleanup(str(root))
    assert os.listdir(root) == ["MH4U.app"]


def test_check_cleans_up_first(github, tmp_path):
    root = installed(tmp_path, {"mh4u.exe": b"new", "mh4u.exe" + update.OLD_SUFFIX: b"old",
                                "mh4u-tools/LICENSE": b"gpl"})
    github.add_release("v0.1.3", {"mh4u-port-windows.zip": windows_zip({"mh4u.exe": b"new"})})
    run("check", "--platform", "windows", "--to", str(root))
    assert not (root / ("mh4u.exe" + update.OLD_SUFFIX)).exists()
