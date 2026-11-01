"""tools/recomp/runtime/recomp.h's RHost, the callbacks a host gives a code library's rc_code_init
(runtime/rc_dll.c), which copies the whole struct as the library was built with it. The native
branch's tools build libraries for Azahar under the same name and RC_HOST_VERSION, with a last
field, preempt_flag, for its RC_NATIVE builds: main's RHost has the same fields in the same order,
so that such a library reads no further than main's host gives it. Main's host (port/src/
arm_recomp.cpp) passes NULL there, and refuses an RC_NATIVE library by its rc_build_mode before
it calls rc_code_init.

The header is built into a small driver with the C compiler found, as tests/test_hook_address.py
builds its own."""
from __future__ import annotations

import os
import shutil
import subprocess

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

CC = next((c for c in ("cc", "clang", "gcc") if shutil.which(c)), None)

# the native branch's RHost, in its order
FIELDS = ["version", "ctx_size", "call", "svc", "trap", "bad_return", "budget", "slow_read", "slow_write",
          "preempt_flag"]

DRIVER = r"""
#include <stddef.h>
#include <stdio.h>

#include "recomp.h"

#define OFFSET(field) printf(" %u", (unsigned)offsetof(RHost, field));

/* a pointer's size, RHost's, then the offsets of the fields named in OFFSETS */
int main(void) {
    printf("%u %u", (unsigned)sizeof(void *), (unsigned)sizeof(RHost));
    OFFSETS
    printf("\n");
    return 0;
}
"""


@pytest.mark.skipif(CC is None, reason="needs a C compiler")
def test_rhost_has_the_native_branchs_fields_in_its_order(tmp_path):
    src = tmp_path / "driver.c"
    src.write_text(DRIVER.replace("    OFFSETS", "    " + " ".join(f"OFFSET({f})" for f in FIELDS)))
    exe = tmp_path / "driver"
    subprocess.run([CC, "-DRC_PAGED", "-I", os.path.join(ROOT, "tools", "recomp", "runtime"), str(src), "-o",
                    str(exe)], check=True)
    out = subprocess.run([str(exe)], capture_output=True, text=True, check=True).stdout
    pointer, size, *offsets = map(int, out.split())
    # two u32, then pointers only, preempt_flag last
    assert offsets == [0, 4] + [8 + pointer * i for i in range(len(FIELDS) - 2)]
    assert size == 8 + pointer * (len(FIELDS) - 2)


def test_the_host_refuses_rc_native_first_and_passes_no_preempt_flag():
    with open(os.path.join(ROOT, "port", "src", "arm_recomp.cpp"), encoding="utf-8") as f:
        text = f.read()
    load = text[text.index("bool ARM_Recomp::LoadCode("):]
    load = load[:load.index("\n}\n")]
    assert load.index('GetSymbol<const u32*>("rc_build_mode")') < load.index("init(&host, &info)")
    assert "/*preempt_flag=*/nullptr}" in load
