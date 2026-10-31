# Ghidra scripts

Run these from Ghidra's Script Manager (add this folder via "Manage Script Directories") or headless with `-postScript`.

| Script | What it does |
|---|---|
| `ExportForAgents.java` | Exports the function list, call graph, strings and (optionally) per-function pseudo-C as text for the agent workflow (`docs/AGENT_WORKFLOW.md`). Verified on Ghidra 12.1.4. |
| `ExportPseudoCParallel.java` | Headless only (`-postScript ExportPseudoCParallel.java <out_dir> [threads] [fresh]`, with `-process <program> -noanalysis -readOnly` on a saved analysis). The pseudo-C of every function (`decomp/<addr>.c`, as ExportForAgents' `all`, same decompiler settings) on several threads (1 to the number of processors), each with its own decompiler. Resumes an interrupted export: files already written are kept, failed decompiles are retried. Resuming also keeps stale output, so after a re-analysis pass `fresh` (or delete `decomp/`). Ends with an error if any function got no file. The contributor's run took under 3 minutes on 12 threads of a 16-core PC. |
| `CreateFunctionsFromList.java` | Creates functions at the addresses in a list, e.g. the output of `tools/find_missing_functions.py` (code only reachable through vtables). Re-run auto-analysis afterwards. |

Write exports to a folder **outside** any git repository (e.g. next to your dump). They are derived from game code and must never be committed.

## Recommended pass order
1. Import the ELF (`ARM:LE:32:v6`, default compiler) and auto-analyze.
2. `ExportForAgents` (pseudo-C: none) → `functions.tsv`.
3. `python3 tools/find_missing_functions.py ... -o missing_functions.txt`
4. `CreateFunctionsFromList` with that file, then **Analysis → Auto Analyze** again.
5. `ExportForAgents` again.
