# N3-16: the check of `saves.py` with the maintainer's real save

The cloud part of N3-16 (`tools/native/saves.py`, `tests/test_native_saves.py`, `docs/SAVES.md`) works on synthetic files. This procedure is its *(with the dump; the save)* item: it fills the constants the tool cannot know without the game's own files, and then imports the maintainer's staged save and extdata into a copy of G3's user folder and checks the round trip. **The local session runs it** on the maintainer's Windows PC, in PowerShell, from the repository checkout; nothing writes into the staged originals or into `build\golden`. The numbers it reads (four per metadata file) go into the tool as numbers only.

The package stays `review` until step 5 is committed.

Each step says what to expect and about how long it takes. After every `py -3` line, `$LASTEXITCODE` must be `0`: PowerShell does not stop on a failing native command, so read it.

**Setup** (5 s). It assumes Git for Windows at `C:\Program Files\Git\usr\bin` (for `diff.exe` and `cmp.exe`; change `$G` if it is elsewhere).

```
Set-Location C:\NoJales\mh4pcport\repo
$Z  = '00000000000000000000000000000000'
$G  = 'C:\Program Files\Git\usr\bin'
$UB = 'build\golden\G3\user-before'
$UT = 'build\n3-16-user'
$SDB = "$UB\sdmc\Nintendo 3DS\$Z\$Z"
$SDT = "$UT\sdmc\Nintendo 3DS\$Z\$Z"
Test-Path "$G\diff.exe"; Test-Path "$G\cmp.exe"
```

*Expect:* `True` and `True`. *Fails if:* either is `False`: find Git's `usr\bin` and set `$G` before going on.

## Step 1 · Read the constants (about 5 s)

```
Get-ChildItem -Recurse -Name "$SDB\extdata\00000000\00001263"
py -3 tools\native\saves.py format-info "$SDB\title\00040000\00126300\data\00000001.metadata"; $LASTEXITCODE
py -3 tools\native\saves.py format-info "$SDB\extdata\00000000\00001263\metadata"; $LASTEXITCODE
```

*Expect:* the listing shows `boss`, `metadata`, `user`, `user\card1` to `user\quest5` and possibly an `icon` file (note whether there is one: the tool never recreates it, and the comparisons below leave it out). Each `format-info` prints one line `TOTAL,DIRS,FILES,DUP` (four numbers, nothing else) and `0`. The extdata folder is named `00001263`, which equals `EXTDATA_ID_DEFAULT`. *Fails if:* a `format-info` ends with an `io` error: that metadata file is not in `user-before`: stop and report.

## Step 2 · Fill the constants (about 5 min)

In `tools/native/saves.py` set `FORMAT_INFO = {"save": (<the four numbers of the first line>), "extdata": (<the four numbers of the second>)}`, rewrite the comment above it (it still says none is filled and none is invented: say that the numbers come from the two metadata files of `build/golden/G3/user-before`, and give nothing else of them), and delete the **Status** paragraph of `docs/SAVES.md` (it only exists because the constants were missing). Then:

```
py -3 -m pytest -q tests\test_native_saves.py; $LASTEXITCODE
```

*Expect:* `70 passed`, `0`, no skipped (the tests that need no format information monkeypatch the dictionary or pass `--format-info`, so they hold with the real numbers). *Fails if:* a test fails: the numbers were typed wrongly (compare with step 1's output).

## Step 3 · Make the test copy, without the save and the extdata (about 10 s)

```
Remove-Item -Recurse -Force $UT -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force build\n3-16-out -ErrorAction SilentlyContinue
Copy-Item -Recurse $UB $UT
Remove-Item -Recurse -Force "$SDT\title\00040000\00126300\data\00000001"
Remove-Item -Force "$SDT\title\00040000\00126300\data\00000001.metadata"
Remove-Item -Recurse -Force "$SDT\extdata\00000000\00001263"
```

*Expect:* no output.

## Step 4 · Import the staged folders, without `--format-info` (about 5 s)

```
py -3 tools\native\saves.py import-save C:\NoJales\mh4pcport\save\00000001 --user $UT --json; $LASTEXITCODE
py -3 tools\native\saves.py import-extdata C:\NoJales\mh4pcport\save\user --user $UT --json; $LASTEXITCODE
```

*Expect:* both print `"ok": true`, `"metadata": "written"`, `"backup": null` and `"removed": []`, and `0`. The save lists 2 files (`system`, `user1`); the extdata lists 10 (`card1` to `card3`, `cardbox`, `otomo1`, `quest1` to `quest5`). The extdata answer has `"extdata_id": "0000000000001263"` and `"extdata_id_source": "constant"` (`"exheader"` with the same id if `$UT\native\game\exheader.bin` exists). *Fails if:* `no_format_info` appears: step 2 was not saved.

## Step 5 · The comparisons (about 10 s)

`diff -rq` prints one `Files ... differ` line per differing text or binary file and nothing when the folders are equal.

(a) The save, against `user-before`:
```
& "$G\diff.exe" -rq "$SDB\title\00040000\00126300\data\00000001" "$SDT\title\00040000\00126300\data\00000001"
& "$G\cmp.exe" -l "$SDB\title\00040000\00126300\data\00000001\system" "$SDT\title\00040000\00126300\data\00000001\system" | Measure-Object -Line
```
*Expect:* exactly one line, naming `system` (G3's `user-before` holds `build\n0-10-save-cpp-off`, whose `system` differs from the staged save in 508 of its 512 bytes, `golden.md`, *The save*), and `Lines : 508` from the second command; `user1` must not appear.

(b) The extdata files, and `boss`:
```
& "$G\diff.exe" -rq "$SDB\extdata\00000000\00001263\user" "$SDT\extdata\00000000\00001263\user"
Test-Path "$SDT\extdata\00000000\00001263\boss"
```
*Expect:* the first prints nothing; the second `True`.

(c) The metadata files (`cmp -l` counts from 1; the 16-byte `<IIIB3x` layout has its three padding bytes at 1-based offsets 14, 15 and 16):
```
& "$G\cmp.exe" -l "$SDB\title\00040000\00126300\data\00000001.metadata" "$SDT\title\00040000\00126300\data\00000001.metadata"
& "$G\cmp.exe" -l "$SDB\extdata\00000000\00001263\metadata" "$SDT\extdata\00000000\00001263\metadata"
```
*Expect:* no output, or only offsets 14, 15 and 16 (N1-13's note: the padding bytes may differ). Any other offset means the constants were typed wrongly.

(d) Export, and compare with the staged originals:
```
py -3 tools\native\saves.py export-save --user $UT build\n3-16-out\save --json; $LASTEXITCODE
py -3 tools\native\saves.py export-extdata --user $UT build\n3-16-out\user --json; $LASTEXITCODE
& "$G\diff.exe" -rq build\n3-16-out\save C:\NoJales\mh4pcport\save\00000001
& "$G\diff.exe" -rq build\n3-16-out\user C:\NoJales\mh4pcport\save\user
```
*Expect:* both exports print `"ok": true` and `"left": []` and `0`; both `diff` calls print nothing. (Step 3 removed `build\n3-16-out`, so the export folders are new and a second run starts from step 3.)

## What to report, and when it fails

Report in the PR or the chat, text only: each step's result (`as expected`, or the output), the four numbers of each metadata file go into the commit of step 2, and whether the extdata root had an `icon` file. When a step fails, stop and send the step's number, the command's output and, for 5 (a), the number of lines `cmp -l` printed. Then commit step 2's changes (`saves.py`, `docs/SAVES.md`), set N3-16 `done` in `docs/native/STATUS.md` and drop the `dump-check pending` marker.
