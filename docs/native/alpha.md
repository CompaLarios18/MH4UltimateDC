# The native alpha: tester reports

`mh4u-native` is an opt-in alpha ([`wp/N3.md`](wp/N3.md), D67): `mh4u` (the Azahar-based build) stays the default. This page is for the people who try it. It says what a report is, what is in it and what never is (D69), how to make one, how to send it, and how the reports are read. Nothing in it is uploaded by the program: a report is a text file in your own user folder, you read it, and you attach it to a GitHub issue yourself.

*Status of the pieces (N3-13 wrote the report code, the checker, the issue form and this page): the crash file and `report::Make` exist as code with tests; the program calls them from N3-30 (`--make-report`, the offer after a crash), the launcher's button is N3-19, and the issue form is offered by GitHub once it is on the default branch (N3-28). Until then these steps describe what the alpha will do.*

## What a report is

A report is one text file, `<user folder>/log/report-<number>.txt`. Its first line is always

```
mh4u-native alpha report v1
```

(that line is how issues with a report are found). It is made two ways: **after a crash** (`mh4u-native` finds a crash file from the last run and offers to make one at the next start) and **on demand** (`mh4u-native --make-report`, or the report button on the launcher's Home screen). Making one writes the file and tells you where it is; it does not open a connection.

## What a report holds, and what it never holds

| A report holds | Where it comes from |
|---|---|
| The versions | the app's commit, and the build record of the code library (its size, the sizes of its code, how many code mods it was built with and their kinds; never the mods' hashes or any path) |
| The system | OS and version, CPU model, logical cores, RAM, GPU name, driver version, Vulkan version, the display modes |
| The settings, by an allowlist | every key of `[Game]`, `[Display]`, `[Audio]` and `[Controls]` in `mh4u-app.ini`, and every key of `mh4u.ini`; a value that is a file or folder is shown as `<path>` |
| The last run's status | the one line the program printed when it ended (for example the exit code and the frames it ran) |
| The newest crash file | the signal or exception, the faulting address, up to 32 stack frames as `module+offset` (module file names only), the build's commit, and the game frame and scene the program had published |
| The last 500 log lines at Info or above | `<user folder>/log/mh4u_native_log.txt` |

| A report never holds | How that is kept |
|---|---|
| Memory contents, screenshots, captures, saves, game files, game text | the report is made from the files above and nothing else; lines that look like hex dumps, disassembly, byte arrays or very long blobs of base64 or hex are dropped, and no line keeps 64 or more hex digits in a row |
| File paths of your PC or of the game | every absolute path (a drive path such as `C:\...`, a `\\server\share` path, a Unix path of two or more parts) becomes `<path>` |
| Your PC's user name, your console's user name | replaced by `<os-user>` and `<console-user>`; your user folder and home folder, where they stand alone, by `<user>` and `<home>` |
| Console ids, MAC addresses, IP addresses | `console id <16 hex digits>` becomes `console id <console-id>`; MACs become `<mac>`; IPv4 and IPv6 addresses, with or without a port, become `<address>` |
| Room invites and online settings | `mh4u://...` becomes `<invite>`; `[Multiplayer]` and `[Mods]` of `mh4u-app.ini` are never read: no nickname, join, password, host, lobby, token, room or last room, no catalog address |
| Trace dumps | of the log lines that look like service or syscall traces (`svc=0x`, `cmd=0x`, ...) only the newest 50 stay; the older ones become one line `<n> trace-like lines left out` |

Two limits you should know. A four-part number such as `1.2.3.4` looks like an IPv4 address and is hidden even when it is a driver version. A path whose last part has a space in its name (`C:\Games\My Game.cxi`) keeps what follows the space; the folders before it, with their spaces, are hidden. This is why you read the report before you send it.

## Reading and checking a report

Open the file in any text editor. It is a few hundred lines. Delete any line you do not want to share (the checker below does not mind). If you have Python 3 and a copy of this repository, check it the way the maintainers do:

```
python3 tools/native/report_check.py <your report> --name <your PC's user name> --name <your console's user name>
```

It prints `ok` and exits 0, or names the rule and the line numbers (never the text of a line) and exits 1. The rules are: the first line is the marker; at most 650 lines; no absolute path, given name, console id, MAC or IP address, invite, `password`/`token`/`join`/`lobby`/`last_room`/`nickname` setting, run of 64 hex digits, hex dump or disassembly, long blob; at most 50 trace-like lines. If the checker refuses a report you made with `mh4u-native`, that is a bug of the program: do not send the report, and open an issue that says only the rule's name and the line numbers.

## Sending it

1. Open **New issue** on the project's page and choose **Native alpha report**.
2. Fill in the form: the version (the `app commit` line of the report), your platform, what happened, and the steps just before it.
3. Drag the report file into the form's **The report file** box, and tick both checks. The first one says you read the report and it holds none of the things in the table above.
4. Never attach anything else of the game or of your PC: no screenshots, no save files, no game files, no captures. If a picture would help, describe it in words.

You can also paste the first lines of the report if you do not want to attach the file; the maintainers will ask for the rest if they need it.

## What to expect afterwards (the triage routine)

N3-28 runs the alpha's two-week window and fills the table below; the routine it follows:

1. Search the issues for the marker `mh4u-native alpha report v1`.
2. Run `tools/native/report_check.py` on each attached report. A report the checker refuses is not read further: the issue gets a comment saying which rule, and asking for the attachment to be removed.
3. Read the crash frames: `module+offset` is the offset from the module's load address, so `llvm-symbolizer --obj=<the module of the same build> 0x<offset>` names the function when the build has debug information (the Windows artifact does).
4. Group reports by the first frame, the game frame's scene and the platform; one fix work package per group, linked from the issue.
5. Answer in the issue: whether it is a known problem, what the fix is, which build has it.

| Issue | Marker found | Platform | Area | First frame | Scene | Verdict | Fix |
|---|---|---|---|---|---|---|---|
| *(filled in by N3-28)* | | | | | | | |

## For the developers

- The code is `native/app/report/` (`report.h` is the interface): `report::Install(user_dir)` before `Runtime::Boot`, `report::SetGameState(frame, scene)` from the app each frame, `report::Make`, `report::Redact`, `report::Pending`, `report::CollectNames`. Installing it, the options and the offer after a crash are N3-30's.
- The crash handler is async-signal-safe (a file opened in advance, a fixed buffer, no allocation, no `fmt`, no logging; Linux resolves `module+offset` from `/proc/self/maps`, macOS with `dladdr`). It chains to the handler installed before it, and the guest memory's handler, installed at boot, chains to it. On Windows it is `SetUnhandledExceptionFilter`, which runs after the vectored handlers. An orderly exit removes the crash file when nothing was written to it; an empty `crash-*.txt` left by a killed process is not a crash (`report::Pending`).
- `tools/native/report_check.py` is the gate and `tests/test_native_report_check.py` its tests; `native/app/report/patterns.h` copies the trace and listing patterns of `tools/native/check_no_game_data.py`, and a test fails when they drift apart.
- Windows and macOS: the Windows crash path (`crash_win32.cpp`) and macOS's SIGBUS and `dladdr` are built and run only by the `app-windows` and macOS jobs of `.github/workflows/native-app.yml`.
