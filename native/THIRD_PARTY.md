# Third-party code in `native/`

One row per vendored library and per piece copied from Azahar (D6). Only the files the build
needs are vendored, each library with its licence file, unmodified. Only the orchestrator edits
this file; a work package that adds a row proposes it in its report.

## Vendored libraries

| Library | Version | Upstream tag | Licence | Where | Files | Used for |
|---|---|---|---|---|---|---|
| doctest | 2.4.11 | `v2.4.11` (github.com/doctest/doctest) | MIT | `native/third_party/doctest/` | `doctest/doctest.h` (single header), `LICENSE.txt` | unit tests (`native/tests`) |
| {fmt} | 11.2.0 | `11.2.0` (github.com/fmtlib/fmt), the newest 11.x release | MIT (with fmt's optional exception for embedded binary output) | `native/third_party/fmt/` | `include/fmt/{base.h,core.h,format.h,format-inl.h}`, `src/format.cc`, `LICENSE` | formatting for `MH4N_LOG` and messages; `src/format.cc` is built as the static library `mh4n_fmt` |

Updating one: replace the files from the new tag, keep the file list, change the row, build, run
the tests.

## Pieces copied from Azahar

Each copied file starts with
`// Derived from Azahar <path> at <commit> (GPL-2.0-or-later or GPL-3.0-or-later, as the file says)`
and gets a row here: `native/<file>` ← `<Azahar path>`, the pinned commit (`port/azahar/COMMIT`).

| File | Azahar source | Commit | Licence (as the source says) | What |
|---|---|---|---|---|
| `native/src/memory/fcram_regions.cpp` | `src/core/hle/kernel/memory.cpp` | `5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6` | GPL-2.0-or-later | the region size table and `MemoryRegionInfo` (`Reset`, `HeapAllocate`, both `LinearAllocate`, `RLinearAllocate`, `Free`), on a `std::map` of free intervals instead of `boost::icl` |
| `native/src/memory/guest_memory.cpp` | `src/core/hle/kernel/vm_manager.cpp`, `src/video_core/gpu.cpp` | `5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6` | GPL-2.0-or-later | the splitting and merging of the block list (`CarveVMA`, `CarveVMARange`, `SplitVMA`, `MergeAdjacent`, `ChangeMemoryState`, `UnmapRange`, `ReprotectRange`) and `GPU::VirtualToPhysicalAddress` |
| `native/include/mh4n/memory.h` | `src/core/memory.h`, `src/core/hle/kernel/errors.h` | `5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6` | GPL-2.0-or-later | the address-space constants and the result code values |
