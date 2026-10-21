#!/usr/bin/env python3
"""Updates an installed package of the MH4U PC port (the launcher's Update button) from the
repository's GitHub releases.

  update.py [--json] check --platform P --commit SHA [--to DIR]
      Is there a release newer than the app built from SHA? (GitHub compares the release's
      commit with SHA: "update" when the release is ahead, "current", "newer" when the app is
      ahead of it, "unknown" when GitHub doesn't know SHA.) With --to, first removes what an
      earlier update left behind there.
  update.py [--json] install --platform P --to DIR [--tag TAG]
      Downloads the release's package for P (windows, linux, macos), checks its SHA-256 against
      the one GitHub gives, unpacks it next to DIR and puts its files over DIR's: a file in use
      (the running app on Windows) is renamed out of the way to NAME.mh4u-old, removed the next
      time. Files of the package's that DIR has and the new package doesn't are left alone, and
      nothing outside DIR is touched (the settings, saves and code library are in Azahar's user
      folder).
  update.py cleanup --to DIR

--json: one JSON object on stdout ({"ok": true, ...} or {"ok": false, "error": {"message": ...}}),
with progress lines before it that start with "@" (as tools/mods.py prints them). MH4U_UPDATE_API
replaces https://api.github.com (tests), MH4U_UPDATE_REPO the repository.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import posixpath
import shutil
import stat
import sys
import tarfile
import time
import urllib.error
import urllib.request
import zipfile

REPO = "CompaLarios18/MH4UltimateDC"
API = "https://api.github.com"
USER_AGENT = "mh4u-port-updater"
ASSETS = {
    "windows": "mh4u-port-windows.zip",
    "linux": "mh4u-port-linux.tar.gz",
    "macos": "mh4u-port-macos.zip",
}
OLD_SUFFIX = ".mh4u-old"
WORK = ".mh4u-update"  # inside DIR: the download and the unpacked package, removed after
TIMEOUT = 30


class UpdateError(Exception):
    pass


class Progress:
    """Progress lines for the launcher: "@{...}" with --json, plain text otherwise."""

    def __init__(self, as_json: bool, interval: float = 0.25):
        self.as_json, self.interval, self.last = as_json, interval, 0.0

    def stage(self, message: str) -> None:
        self._emit({"event": "stage", "message": message}, force=True)

    def update(self, what: str, done: int, total: int | None) -> None:
        now = time.monotonic()
        if now - self.last < self.interval and (total is None or done < total):
            return
        self.last = now
        self._emit({"event": "download", "what": what, "done": done, "total": total}, force=False)

    def _emit(self, data: dict, force: bool) -> None:
        if self.as_json:
            sys.stdout.write("@" + json.dumps(data) + "\n")
        elif force:
            sys.stderr.write(data.get("message", "") + "\n")
        else:
            total = f" of {data['total'] // 1048576} MB" if data.get("total") else ""
            sys.stderr.write(f"\r{data['what']}: {data['done'] // 1048576} MB{total}")
        (sys.stdout if self.as_json else sys.stderr).flush()


# ── GitHub ──

def api_base() -> str:
    return os.environ.get("MH4U_UPDATE_API", API).rstrip("/")


def repo() -> str:
    return os.environ.get("MH4U_UPDATE_REPO", REPO)


def get_json(path: str):
    url = f"{api_base()}/repos/{repo()}/{path}"
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT,
                                                   "Accept": "application/vnd.github+json"})
    try:
        with urllib.request.urlopen(request, timeout=TIMEOUT) as response:
            return json.load(response)
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None
        if e.code == 403:
            raise UpdateError("GitHub refused the request (too many in an hour from this address?): "
                              "try again later") from e
        raise UpdateError(f"GitHub answered {e.code} for {url}") from e
    except (urllib.error.URLError, OSError) as e:
        raise UpdateError(f"GitHub couldn't be reached: {getattr(e, 'reason', e)}") from e


def release(tag: str | None):
    data = get_json(f"releases/tags/{tag}" if tag else "releases/latest")
    if not isinstance(data, dict) or "tag_name" not in data:
        raise UpdateError(f"no release {tag}" if tag else "the repository has no release yet")
    return data


def choose_asset(rel: dict, platform: str) -> dict:
    name = ASSETS[platform]
    for asset in rel.get("assets", []):
        if asset.get("name") == name:
            return asset
    raise UpdateError(f"release {rel.get('tag_name')} has no {name}")


def asset_sha256(asset: dict) -> str | None:
    digest = asset.get("digest") or ""
    return digest.split(":", 1)[1].lower() if digest.startswith("sha256:") else None


def compare(commit: str, tag: str) -> str:
    """update, current, newer or unknown: where the release's commit is from `commit`'s."""
    if not commit:
        return "unknown"
    data = get_json(f"compare/{commit}...{tag}")
    if not isinstance(data, dict):
        return "unknown"
    return {"ahead": "update", "identical": "current", "behind": "newer",
            "diverged": "update"}.get(data.get("status", ""), "unknown")


# ── Files ──

def cleanup(root: str) -> list[str]:
    """Removes what an earlier update left in `root`: renamed files and its work folder."""
    removed = []
    for folder, dirs, files in os.walk(root):
        names = files + dirs
        dirs[:] = [d for d in dirs if d != WORK and OLD_SUFFIX not in d]
        for name in names:
            if OLD_SUFFIX in name:
                path = os.path.join(folder, name)
                try:
                    if os.path.isdir(path) and not os.path.islink(path):
                        shutil.rmtree(path)
                    else:
                        os.remove(path)
                    removed.append(os.path.relpath(path, root))
                except OSError:
                    pass  # still in use: the next time
    shutil.rmtree(os.path.join(root, WORK), ignore_errors=True)
    return removed


def download(url: str, path: str, size: int | None, progress: Progress) -> None:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, "Accept": "application/octet-stream"})
    try:
        with urllib.request.urlopen(request, timeout=TIMEOUT) as response, open(path, "wb") as out:
            total = size or int(response.headers.get("Content-Length") or 0) or None
            done = 0
            while True:
                chunk = response.read(1 << 20)
                if not chunk:
                    break
                out.write(chunk)
                done += len(chunk)
                progress.update("Downloading the update", done, total)
    except (urllib.error.URLError, OSError) as e:
        raise UpdateError(f"the download stopped: {getattr(e, 'reason', e)}") from e
    if size and os.path.getsize(path) != size:
        raise UpdateError("the download is incomplete: try again")


def sha256_of(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def safe_member(name: str) -> str | None:
    """The member's path without the package's top folder, or None if it would leave the folder."""
    parts = [p for p in name.replace("\\", "/").split("/") if p not in ("", ".")]
    if not parts or ".." in parts or ":" in parts[0] or name.startswith(("/", "\\")):
        return None
    return "/".join(parts[1:])  # mh4u-port-windows/mh4u.exe -> mh4u.exe


def link_escapes(rel: str, link: str) -> bool:
    """Whether a link at `rel` (in the package) to `link` points outside the package."""
    if link.startswith(("/", "\\")) or ":" in link.split("/")[0]:
        return True
    target = posixpath.normpath(posixpath.join(posixpath.dirname(rel), link))
    return target == ".." or target.startswith("../")


def unpack(archive: str, dest: str) -> int:
    """Unpacks the package (its top folder dropped) into `dest`; the number of files."""
    count = 0
    os.makedirs(dest, exist_ok=True)
    if archive.endswith(".zip"):
        with zipfile.ZipFile(archive) as z:
            for info in z.infolist():
                rel = safe_member(info.filename)
                if rel is None:
                    raise UpdateError(f"the package has a file outside its folder: {info.filename}")
                if not rel:
                    continue
                target = os.path.join(dest, *rel.split("/"))
                mode = info.external_attr >> 16
                if info.is_dir():
                    os.makedirs(target, exist_ok=True)
                    continue
                os.makedirs(os.path.dirname(target), exist_ok=True)
                if stat.S_ISLNK(mode):
                    # a Mac app's frameworks link their versions (ditto keeps the links)
                    link = z.read(info).decode()
                    if link_escapes(rel, link):
                        raise UpdateError(f"the package has a link outside its folder: {info.filename}")
                    os.symlink(link, target)
                else:
                    with z.open(info) as src, open(target, "wb") as out:
                        shutil.copyfileobj(src, out)
                    if mode & 0o111:
                        os.chmod(target, (mode & 0o777) | 0o200)
                count += 1
    else:
        with tarfile.open(archive) as t:
            for member in t.getmembers():
                rel = safe_member(member.name)
                if rel is None or member.isdev():
                    raise UpdateError(f"the package has a file outside its folder: {member.name}")
                if not rel:
                    continue
                target = os.path.join(dest, *rel.split("/"))
                if member.isdir():
                    os.makedirs(target, exist_ok=True)
                elif member.issym():
                    if link_escapes(rel, member.linkname):
                        raise UpdateError(f"the package has a link outside its folder: {member.name}")
                    os.makedirs(os.path.dirname(target), exist_ok=True)
                    os.symlink(member.linkname, target)
                    count += 1
                elif member.isfile():
                    os.makedirs(os.path.dirname(target), exist_ok=True)
                    with t.extractfile(member) as src, open(target, "wb") as out:
                        shutil.copyfileobj(src, out)
                    os.chmod(target, (member.mode & 0o777) | 0o200)
                    count += 1
    return count


def move_aside(path: str) -> str:
    """Renames `path` to PATH.mh4u-old (or .mh4u-old2, ... when an older one is still in use)."""
    aside, n = path + OLD_SUFFIX, 1
    while os.path.lexists(aside):
        try:
            remove(aside)
        except OSError:
            n += 1
            aside = f"{path}{OLD_SUFFIX}{n}"
    os.rename(path, aside)
    return aside


def remove(path: str) -> None:
    if os.path.isdir(path) and not os.path.islink(path):
        shutil.rmtree(path)
    else:
        os.remove(path)


def put_in_place(new: str, root: str, replace=os.replace) -> tuple[int, list[str]]:
    """Moves the files of `new` over `root`'s. A Mac app (a top-level NAME.app folder) is swapped
    whole, its old one renamed to NAME.app.mh4u-old (its code signature covers every file in it).
    A file that can't be replaced (in use, on Windows) is renamed to NAME.mh4u-old first.
    Returns how many were put in place and the ones renamed."""
    moved, renamed = 0, []
    for name in sorted(os.listdir(new)):
        src = os.path.join(new, name)
        if name.endswith(".app") and os.path.isdir(src) and not os.path.islink(src):
            dst = os.path.join(root, name)
            if os.path.lexists(dst):
                move_aside(dst)
                renamed.append(name)
            replace(src, dst)
            moved += 1
    for folder, dirs, files in os.walk(new):
        rel_folder = os.path.relpath(folder, new)
        target_folder = root if rel_folder == "." else os.path.join(root, rel_folder)
        os.makedirs(target_folder, exist_ok=True)
        for name in files + [d for d in dirs if os.path.islink(os.path.join(folder, d))]:
            src, dst = os.path.join(folder, name), os.path.join(target_folder, name)
            if os.path.islink(src) and os.path.lexists(dst):
                remove(dst)  # a link replaces whatever was there
            try:
                replace(src, dst)
            except PermissionError:
                move_aside(dst)
                renamed.append(os.path.relpath(dst, root))
                replace(src, dst)
            moved += 1
    return moved, renamed


def app_path(root: str, platform: str) -> str:
    """What to start once the update is in place."""
    return os.path.join(root, {"windows": "mh4u.exe", "linux": "mh4u.AppImage", "macos": "MH4U.app"}[platform])


# ── Commands ──

def cmd_check(args, progress: Progress) -> dict:
    if args.to:
        cleanup(args.to)
    rel = release(None)
    asset = choose_asset(rel, args.platform)
    status = compare(args.commit, rel["tag_name"])
    return {"status": status, "tag": rel["tag_name"], "name": rel.get("name") or rel["tag_name"],
            "url": rel.get("html_url", ""), "published": rel.get("published_at", ""),
            "notes": (rel.get("body") or "")[:4000], "size": asset.get("size"),
            "prerelease": bool(rel.get("prerelease"))}


def cmd_install(args, progress: Progress) -> dict:
    root = os.path.abspath(args.to)
    if not os.path.isdir(os.path.join(root, "mh4u-tools")):
        raise UpdateError(f"{root} isn't a package of the port (it has no mh4u-tools folder): a build from "
                          "the source updates with git instead")
    progress.stage("Looking for the release")
    rel = release(args.tag)
    asset = choose_asset(rel, args.platform)
    work = os.path.join(root, WORK)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    archive = os.path.join(work, asset["name"])
    progress.stage(f"Downloading {rel['tag_name']}")
    download(asset["browser_download_url"], archive, asset.get("size"), progress)
    expected = asset_sha256(asset)
    if expected:
        progress.stage("Checking the download")
        if sha256_of(archive) != expected:
            raise UpdateError("the download doesn't match the release's checksum: try again")
    progress.stage("Unpacking")
    new = os.path.join(work, "new")
    files = unpack(archive, new)
    if not files:
        raise UpdateError("the package is empty")
    progress.stage("Putting the new files in place")
    moved, renamed = put_in_place(new, root)
    shutil.rmtree(work, ignore_errors=True)
    return {"tag": rel["tag_name"], "name": rel.get("name") or rel["tag_name"], "files": moved,
            "renamed": renamed, "app": app_path(root, args.platform), "verified": bool(expected)}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--json", action="store_true")
    sub = parser.add_subparsers(dest="command", required=True)
    check = sub.add_parser("check")
    check.add_argument("--platform", choices=sorted(ASSETS), required=True)
    check.add_argument("--commit", default="")
    check.add_argument("--to")
    install = sub.add_parser("install")
    install.add_argument("--platform", choices=sorted(ASSETS), required=True)
    install.add_argument("--to", required=True)
    install.add_argument("--tag")
    clean = sub.add_parser("cleanup")
    clean.add_argument("--to", required=True)
    args = parser.parse_args(argv)
    progress = Progress(args.json)
    try:
        if args.command == "check":
            result = cmd_check(args, progress)
        elif args.command == "install":
            result = cmd_install(args, progress)
        else:
            result = {"removed": cleanup(args.to)}
    except UpdateError as e:
        if args.json:
            print(json.dumps({"ok": False, "error": {"message": str(e)}}))
        else:
            print(f"update: {e}", file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps(dict(result, ok=True)))
    else:
        for key, value in result.items():
            print(f"{key}: {value}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
