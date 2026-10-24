"""tools/native/vk_layer_run.py (N2-25): the parser of the layer's output on synthetic logs, the layer lookup, and
the command line against a fake ctest (no Vulkan, no build)."""
import json
import os
import stat
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
TOOL = ROOT / "tools" / "native" / "vk_layer_run.py"
sys.path.insert(0, str(ROOT / "tools" / "native"))

import vk_layer_run as vlr  # noqa: E402

SYNC = (
    "Validation Error: [ SYNC-HAZARD-READ-AFTER-WRITE ] Object 0: handle = 0x1, type = VK_OBJECT_TYPE_QUEUE; "
    "| MessageID = 0x5c0ec5d6 | vkQueueSubmit(): Hazard READ_AFTER_WRITE for entry 0, VkCommandBuffer 0x2.\n"
)
VUID = (
    "Validation Error: [ VUID-VkImageViewCreateInfo-image-01762 ] Object 0: handle = 0x3 | MessageID = 0x1 | "
    "vkCreateImageView(): bad. The Vulkan spec states: x (https://docs.vulkan.org/spec/latest/chapters/resources.html#VUID-VkImageViewCreateInfo-image-01762)\n"
)
WARNING = "Validation Warning: [ UNASSIGNED-CoreValidation-DrawState-InvalidImageLayout ] Object 0: x | MessageID = 0x2 | y\n"
BEST = "Validation Warning: [ BestPractices-pipeline-stage-flags ] Object 0: x | MessageID = 0x3 | y\n"
CLEAN = "[==] doctest\nrhi_plume_test: device: llvmpipe (LLVM 20.1.2, 256 bits)\n[doctest] Status: SUCCESS!\n"


def test_clean_log_has_no_message():
    assert vlr.parse_layer_output(CLEAN) == {}


def test_messages_are_counted_by_id_not_by_url():
    counts = vlr.parse_layer_output(SYNC + SYNC + SYNC + VUID + WARNING + CLEAN)
    assert counts == {
        "SYNC-HAZARD-READ-AFTER-WRITE": 3,
        "VUID-VkImageViewCreateInfo-image-01762": 1,  # the spec URL on the same line is not a second message
        "UNASSIGNED-CoreValidation-DrawState-InvalidImageLayout": 1,
    }


def test_a_bare_id_line_is_a_message():
    counts = vlr.parse_layer_output("something: VUID-vkCmdDraw-None-02699 happened\n")
    assert counts == {"VUID-vkCmdDraw-None-02699": 1}


def test_a_header_without_an_id_is_a_message():
    assert vlr.parse_layer_output("Validation Error: [ ] something\n") == {"(no id)": 1}


def test_best_practices_is_a_message_and_is_called_out(capsys):
    counts = vlr.parse_layer_output(BEST)
    assert counts == {"BestPractices-pipeline-stage-flags": 1}
    assert vlr.report(counts) == 1
    assert "settings were not applied" in capsys.readouterr().out


def test_report_prints_ids_with_counts(capsys):
    assert vlr.report(vlr.parse_layer_output(SYNC + SYNC)) == 1
    out = capsys.readouterr().out
    assert "SYNC-HAZARD-READ-AFTER-WRITE  x2" in out
    assert vlr.report(vlr.parse_layer_output(CLEAN)) == 0


def write_manifest(directory: Path, name: str):
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "layer.json").write_text(json.dumps({"file_format_version": "1.2.0", "layer": {"name": name}}))


def test_find_validation_layer(tmp_path):
    assert vlr.find_validation_layer([tmp_path / "missing"]) is None
    write_manifest(tmp_path / "other", "VK_LAYER_MESA_overlay")
    assert vlr.find_validation_layer([tmp_path / "other"]) is None
    write_manifest(tmp_path / "ok", "VK_LAYER_KHRONOS_validation")
    assert vlr.find_validation_layer([tmp_path / "other", tmp_path / "ok"]) == tmp_path / "ok" / "layer.json"


def test_environment():
    env = vlr.layer_environment({"PATH": "x"})
    assert env["VK_INSTANCE_LAYERS"] == "VK_LAYER_KHRONOS_validation"
    assert env["MH4N_REQUIRE_VULKAN"] == "1"
    assert env["VK_LAYER_ENABLES"] == "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT"
    settings = Path(env["VK_LAYER_SETTINGS_PATH"]) / "vk_layer_settings.txt"
    text = "\n".join(l for l in settings.read_text().splitlines() if not l.startswith("#"))
    assert "report_flags = error,warn" in text
    assert "BEST_PRACTICES" not in text and "perf" not in text
    assert "SYNCHRONIZATION_VALIDATION" in text and "duplicate_message_limit = 3" in text


# ---- the command line, with a fake ctest and a fake test binary -------------------------------------------------------

def fake_tools(tmp_path: Path, output: str, status: int = 0) -> dict:
    """A PATH with a `ctest` that prints `output` (and, for -N --show-only, a test list) and a test binary that does too."""
    bindir = tmp_path / "bin"
    bindir.mkdir()
    binary = bindir / "fake_test"
    binary.write_text(f"#!/bin/sh\ncat <<'EOF'\n{output}EOF\nexit {status}\n")
    ctest = bindir / "ctest"
    listing = json.dumps({"tests": [{"name": "fake_test", "command": [str(binary)]}]})
    ctest.write_text(
        "#!/bin/sh\n"
        'case "$*" in *--show-only*) cat <<\'EOF\'\n' + listing + "\nEOF\nexit 0;; esac\n"
        f"cat <<'EOF'\n{output}EOF\nexit {status}\n"
    )
    for p in (binary, ctest):
        p.chmod(p.stat().st_mode | stat.S_IEXEC)
    env = dict(os.environ)
    env["PATH"] = f"{bindir}:{env['PATH']}"
    return env


def run_cli(env: dict, tmp_path: Path, *extra: str, layer: bool = True):
    dirs = []
    if layer:
        write_manifest(tmp_path / "layers", "VK_LAYER_KHRONOS_validation")
        dirs = [str(tmp_path / "layers")]
    else:
        (tmp_path / "nolayers").mkdir(exist_ok=True)
        dirs = [str(tmp_path / "nolayers")]
    return subprocess.run(
        [sys.executable, str(TOOL), "--build", str(tmp_path / "build"), "--layer-manifest-dirs", *dirs, *extra],
        capture_output=True, text=True, env=env,
    )


def test_cli_clean_run_passes(tmp_path):
    r = run_cli(fake_tools(tmp_path, CLEAN), tmp_path, "-R", "^x$")
    assert r.returncode == 0, r.stdout + r.stderr
    assert "no layer message" in r.stdout


def test_cli_layer_message_in_a_passing_test_fails(tmp_path):
    r = run_cli(fake_tools(tmp_path, CLEAN + SYNC, status=0), tmp_path, "-R", "^x$")
    assert r.returncode == 1
    assert "SYNC-HAZARD-READ-AFTER-WRITE  x1" in r.stdout


def test_cli_best_practices_line_fails(tmp_path):
    r = run_cli(fake_tools(tmp_path, BEST), tmp_path, "-R", "^x$")
    assert r.returncode == 1 and "BestPractices-pipeline-stage-flags" in r.stdout


def test_cli_failing_test_fails(tmp_path):
    r = run_cli(fake_tools(tmp_path, CLEAN, status=8), tmp_path, "-R", "^x$")
    assert r.returncode == 1


def test_cli_missing_layer_fails_and_says_how_to_install(tmp_path):
    r = run_cli(fake_tools(tmp_path, CLEAN), tmp_path, "-R", "^x$", layer=False)
    assert r.returncode == 1
    assert "apt install vulkan-validationlayers" in r.stderr


def test_cli_test_case_runs_the_binary_and_reports(tmp_path):
    r = run_cli(fake_tools(tmp_path, CLEAN + SYNC), tmp_path, "-R", "^fake_test$", "--test-case", "control/C5")
    assert r.returncode == 1
    assert "SYNC-HAZARD-READ-AFTER-WRITE" in r.stdout
