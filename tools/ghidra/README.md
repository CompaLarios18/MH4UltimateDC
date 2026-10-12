# Ghidra scripts

Run these from Ghidra's Script Manager (add this folder via "Manage Script Directories") or headless with `-postScript`.

| Script | What it does |
|---|---|
| `ExportForAgents.java` | Exports the function list, call graph, strings and (optionally) per-function pseudo-C as text for the agent workflow (`docs/AGENT_WORKFLOW.md`). **Status: not yet run against a real Ghidra install.** |

Write exports to a folder **outside** any git repository (e.g. next to your dump). They are derived from game code and must never be committed.
