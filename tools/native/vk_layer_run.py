#!/usr/bin/env python3
"""Run the RHI tests under the Khronos validation layer and fail on any layer message (N2-25; docs/native/rhi.md).

    vk_layer_run.py --build DIR -R REGEX [--test-case NAME]

Runs `ctest --test-dir DIR --no-tests=error -R REGEX --output-on-failure` (plus -V, because ctest hides the
output of a passing test and the layer's messages must be seen) with

    VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
    VK_LAYER_SETTINGS_PATH=tools/native/vk_layer/        (errors and warnings only, synchronization validation)
    VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
    MH4N_REQUIRE_VULKAN=1                                (a missing device fails a test instead of skipping it)

as docs/research/rhi-spike-evidence/run.sh ran the patterns. With --test-case NAME the regex must select one test
binary, which is run directly with `--test-case=NAME --no-skip` (the controls, skipped by default, are run this way).

plume registers no debug messenger, so the layer prints its messages to stdout/stderr; the output is captured and
the run fails when a line names a `VUID-`, `SYNC-HAZARD-` or `UNASSIGNED-` ID, or is a `Validation Error` or
`Validation Warning` line. A `BestPractices-` ID fails too, as a sign the settings were not applied. Each message ID is
printed with its count. The run fails, it never skips, when the layer is not installed.

Exit status: 0 clean; 1 a layer message appeared, a test failed, or the layer is missing; 2 bad usage.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SETTINGS_DIR = REPO_ROOT / "tools" / "native" / "vk_layer"
LAYER_NAME = "VK_LAYER_KHRONOS_validation"
SYNC_ENABLE = "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT"
INSTALL_HINT = "apt install vulkan-validationlayers"

# "Validation Error: [ VUID-vkCmdDraw-... ] Object 0: ..." (the layer's default message format)
HEADER = re.compile(r"Validation (Error|Warning): \[ ?([^\]]*?) ?\]")
# an ID named anywhere else on a line, not as the anchor of a specification URL
ID_TOKEN = re.compile(r"(?<![\w#/-])((?:VUID|UNASSIGNED|BestPractices)-[A-Za-z0-9_.-]+|SYNC-HAZARD-[A-Z0-9-]+)")


def parse_layer_output(text: str) -> Counter:
    """Message ID -> count, for every layer message in `text` (empty when the output is clean)."""
    counts: Counter = Counter()
    for line in text.splitlines():
        header = HEADER.search(line)
        if header:
            counts[header.group(2).strip() or "(no id)"] += 1
            continue
        token = ID_TOKEN.search(line)
        if token:
            counts[token.group(1)] += 1
    return counts


def default_manifest_dirs(environ: dict[str, str] | None = None) -> list[Path]:
    env = os.environ if environ is None else environ
    dirs: list[Path] = []
    for entry in env.get("VK_LAYER_PATH", "").split(os.pathsep):
        if entry:
            dirs.append(Path(entry))
    for entry in env.get("XDG_DATA_DIRS", "/usr/local/share:/usr/share").split(os.pathsep):
        if entry:
            dirs.append(Path(entry) / "vulkan" / "explicit_layer.d")
    home = env.get("HOME")
    if home:
        dirs.append(Path(home) / ".local" / "share" / "vulkan" / "explicit_layer.d")
    dirs += [Path("/etc/vulkan/explicit_layer.d"), Path("/usr/share/vulkan/explicit_layer.d")]
    return dirs


def find_validation_layer(dirs: list[Path]) -> Path | None:
    """The manifest of VK_LAYER_KHRONOS_validation in one of `dirs`, or None."""
    for directory in dirs:
        if not directory.is_dir():
            continue
        for manifest in sorted(directory.glob("*.json")):
            try:
                data = json.loads(manifest.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                continue
            layers = data.get("layers", [])
            if "layer" in data:
                layers = [data["layer"]]
            if any(isinstance(layer, dict) and layer.get("name") == LAYER_NAME for layer in layers):
                return manifest
    return None


def layer_environment(base: dict[str, str]) -> dict[str, str]:
    env = dict(base)
    env["VK_INSTANCE_LAYERS"] = LAYER_NAME
    env["VK_LAYER_SETTINGS_PATH"] = str(SETTINGS_DIR)
    env["VK_LAYER_ENABLES"] = SYNC_ENABLE
    env["MH4N_REQUIRE_VULKAN"] = "1"
    return env


def report(counts: Counter) -> int:
    """Print the message IDs with their counts; 1 when there is any message."""
    if not counts:
        print("vk_layer_run: no layer message")
        return 0
    print(f"vk_layer_run: {sum(counts.values())} layer message(s), {len(counts)} distinct ID(s):")
    for message_id, count in sorted(counts.items()):
        print(f"  {message_id}  x{count}")
    if any(i.startswith("BestPractices-") for i in counts):
        print("vk_layer_run: a BestPractices- message means the layer settings were not applied "
              f"(VK_LAYER_SETTINGS_PATH={SETTINGS_DIR})")
    return 1


def run_ctest(build: str, regex: str, env: dict[str, str]) -> tuple[int, str]:
    cmd = ["ctest", "--test-dir", build, "--no-tests=error", "-R", regex, "--output-on-failure", "-V"]
    proc = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    return proc.returncode, proc.stdout


def run_test_case(build: str, regex: str, case: str, env: dict[str, str]) -> tuple[int, str]:
    listing = subprocess.run(
        ["ctest", "--test-dir", build, "-N", "-R", regex, "--show-only=json-v1"],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    if listing.returncode != 0:
        return 2, listing.stdout + listing.stderr
    tests = json.loads(listing.stdout).get("tests", [])
    if len(tests) != 1:
        return 2, f"vk_layer_run: -R {regex!r} selects {len(tests)} tests; --test-case needs exactly one test binary\n"
    command = tests[0]["command"]
    proc = subprocess.run(
        [command[0], f"--test-case={case}", "--no-skip"],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
    )
    return proc.returncode, proc.stdout


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Run the RHI tests under the Khronos validation layer.")
    parser.add_argument("--build", required=True, help="the CMake build directory (ctest --test-dir)")
    parser.add_argument("-R", "--regex", required=True, help="the ctest -R regular expression")
    parser.add_argument("--test-case", help="run one doctest case (a control) of the one binary the regex selects")
    parser.add_argument("--layer-manifest-dirs", nargs="*", help="where to look for the layer's manifest (default: the Vulkan loader's search path); for tests")
    args = parser.parse_args(argv)

    dirs = [Path(d) for d in args.layer_manifest_dirs] if args.layer_manifest_dirs is not None else default_manifest_dirs()
    manifest = find_validation_layer(dirs)
    if manifest is None:
        print(f"vk_layer_run: {LAYER_NAME} is not installed; {INSTALL_HINT}", file=sys.stderr)
        return 1

    env = layer_environment(dict(os.environ))
    if args.test_case:
        code, output = run_test_case(args.build, args.regex, args.test_case, env)
    else:
        code, output = run_ctest(args.build, args.regex, env)
    counts = parse_layer_output(output)
    status = report(counts)
    if code != 0 or status != 0:
        sys.stdout.write(output)
    if code == 2 and not counts:
        return 2
    if code != 0:
        print(f"vk_layer_run: the run exited with status {code}")
        return 1
    return status


if __name__ == "__main__":
    sys.exit(main())
