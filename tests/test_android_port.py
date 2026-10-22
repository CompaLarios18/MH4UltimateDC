"""Android port invariants that need no device or game data.

Two things that are easy to lose in a rebase, a patch regeneration or a refactor:

* the applet JNI callbacks (software keyboard, Mii selector, camera) must run on the JNI
  thread, not on the emulation fiber (see ``docs/research/android-fiber-jni.md``), and
* ``mh4u.ini`` must be read through ``FileUtil`` so Android can find it in the user folder
  (``std::ifstream`` cannot follow the guest path).

The Azahar side lives in ``port/azahar/azahar.patch``; the options read is in this
repository. ``MH4U_AZAHAR_DIR`` (the Azahar tree the patch is applied to) additionally
checks that the patch file matches the working tree, i.e. that it was regenerated.
"""
from __future__ import annotations

import os
import subprocess

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PATCH = os.path.join(ROOT, "port", "azahar", "azahar.patch")
HOOKS = os.path.join(ROOT, "port", "src", "hooks.cpp")


def added_lines() -> dict[str, str]:
    """The added lines of the patch, joined, by the file they belong to."""
    files: dict[str, list[str]] = {}
    current = None
    with open(PATCH, encoding="utf-8") as patch:
        for line in patch:
            line = line.rstrip("\n")
            if line.startswith("diff --git "):
                current = line.split(" b/", 1)[1] if " b/" in line else line
                files[current] = []
            elif current is not None and line.startswith("+") and not line.startswith("+++"):
                files[current].append(line[1:])
    return {path: "\n".join(lines) for path, lines in files.items()}


def test_patch_defines_the_jni_executor():
    utils = added_lines().get("src/common/android_utils.cpp", "")
    assert "RunOnJniThread" in utils
    assert "JniThreadMain" in utils
    assert "std::thread" in utils and "condition_variable" in utils
    # primed on a regular thread at load and at directory init, not from the fiber
    assert "g_cached_build_flavor" in utils and "CacheUserDirectory" in utils


def test_patch_runs_applet_jni_on_the_jni_thread():
    files = added_lines()
    # the software keyboard: the dialog (Execute) and the error dialog (ShowError)
    swkbd = files.get("src/android/app/src/main/jni/applets/swkbd.cpp", "")
    assert swkbd.count("AndroidUtils::RunOnJniThread") >= 2
    # the Mii picker and the camera file picker/loader
    for path in ("src/android/app/src/main/jni/applets/mii_selector.cpp",
                 "src/android/app/src/main/jni/camera/still_image_camera.cpp"):
        assert "AndroidUtils::RunOnJniThread" in files.get(path, ""), path


def test_patch_formats_save_data_or_reports_failure():
    savedata = added_lines().get("src/core/file_sys/archive_source_sd_savedata.cpp", "")
    assert "ResultUnknown" in savedata  # no unconditional ResultSuccess
    assert "Common::GetLastErrorMsg" in savedata


def test_options_read_uses_fileutil():
    """LoadOptions must open mh4u.ini through FileUtil, which translates the guest path."""
    with open(HOOKS, encoding="utf-8") as f:
        text = f.read()
    start = text.index("Options LoadOptions(")
    end = text.index("const std::pair<const char*, const char*> env[]", start)
    # comments may mention std::ifstream; only the code counts
    body = "\n".join(line.split("//", 1)[0] for line in text[start:end].splitlines())
    assert "FileUtil::IOFile" in body
    assert "ReadLine" in body
    assert "std::ifstream" not in body


def test_widescreen_match_and_android_first_run():
    """widescreen = match takes the frontend's window, and Android writes its first file."""
    with open(HOOKS, encoding="utf-8") as f:
        hooks = f.read()
    assert "widescreen_match" in hooks
    assert 'value == "match"' in hooks
    assert "void SetWindowSize(" in hooks

    with open(os.path.join(ROOT, "port", "src", "frontend.cpp"), encoding="utf-8") as f:
        frontend = f.read()
    assert "void SetWindowSize(" in frontend
    # the Android first run: the template with match, written when mh4u.ini is missing
    assert "OptionsTemplate(true)" in frontend
    assert '"widescreen = match"' in frontend

    # the Android frontend passes the surface it knows before the port installs
    native = added_lines().get("src/android/app/src/main/jni/native.cpp", "")
    assert "Port::SetWindowSize" in native


@pytest.mark.skipif(not os.environ.get("MH4U_AZAHAR_DIR"),
                    reason="set MH4U_AZAHAR_DIR to the patched Azahar tree")
def test_patch_matches_the_azahar_tree():
    subprocess.run(["git", "-C", os.environ["MH4U_AZAHAR_DIR"], "apply", "--check", "--reverse",
                    PATCH], check=True)
