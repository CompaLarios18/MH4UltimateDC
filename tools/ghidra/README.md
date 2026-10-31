# Ghidra scripts

Run these from Ghidra's Script Manager (add this folder via "Manage Script Directories") or headless with `-postScript`.

| Script | What it does |
|---|---|
| `ExportForAgents.java` | Exports the function list, call graph, strings and (optionally) per-function pseudo-C as text for the agent workflow (`docs/AGENT_WORKFLOW.md`). Verified on Ghidra 12.1.4. |
| `ExportPseudoCParallel.java` | The pseudo-C of every function (`decomp/<addr>.c`, as ExportForAgents' `all`) on several threads, each with its own decompiler; skips files already written, so an interrupted export resumes. Run with `-process <program> -noanalysis -readOnly` on a saved analysis; MH4U (about 19,000 functions) took under 3 minutes on 12 threads of a 16-core PC, after a 16-minute analysis. |
| `CreateFunctionsFromList.java` | Creates functions at the addresses in a list, e.g. the output of `tools/find_missing_functions.py` (code only reachable through vtables). Re-run auto-analysis afterwards. |

Write exports to a folder **outside** any git repository (e.g. next to your dump). They are derived from game code and must never be committed.

## Recommended pass order
1. Import the ELF (`ARM:LE:32:v6`, default compiler) and auto-analyze.
2. `ExportForAgents` (pseudo-C: none) → `functions.tsv`.
3. `python3 tools/find_missing_functions.py ... -o missing_functions.txt`
4. `CreateFunctionsFromList` with that file, then **Analysis → Auto Analyze** again.
5. `ExportForAgents` again.
