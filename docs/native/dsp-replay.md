# The DSP capture replay (N3-04)

`mh4n_dsp_replay` takes an MDSP capture of the reference runner (`mh4u_runner --capture-dsp START:FRAMES:FILE`,
the format of [`tools/native/capfmt.py`](../../tools/native/capfmt.py), N0-06's *As built*) and replays it
through our own HLE DSP ([`dsp.md`](dsp.md), `native/src/dsp_hle/`) frame by frame, with the captured inputs. It
compares the mix our DSP makes with the one the reference made, and the source status blocks our DSP writes
with the ones the reference's next frame holds. This is how ROADMAP §6's "final mix at SNR ≥ 60 dB" is
measured (D75); decoding and interpolation bit-identity is checked elsewhere (our own programs, N3-01, and the
harness that compiles Azahar's code, N3-02/N3-03).

The logic is the module `native/src/dsp_replay/` (`mh4n_dsp_replay`); the program in `native/tools/dsp_replay/` is
only `main`. The tests are `native/tests/dsp_replay_test.cpp` (captures the test writes from our own DSP) and
`tests/test_native_dsp_replay.py` (a capture made by Azahar's own DSP code).

```
mh4n_dsp_replay CAPTURE.mdsp [--report FILE]
```

| | |
|---|---|
| `CAPTURE.mdsp` | the capture |
| `--report FILE` | also write the result lines (settling, every window, every judged frame that is not bit-exact, the verdict line) to FILE; the folder it is in is created when missing |
| `--help` | the usage |

Exit status: **0** `PASS`, **1** `FAIL`, **2** a usage error or a capture that cannot be read (not a capture, a
truncated record, no frames, a frame without its output record, a sample read without bytes whose key no earlier
record gave, bytes that do not match their CRC). The last line of the output is always the verdict:

```
dsp-replay: <n> frames judged of <total>, <m> bit-exact, snr <dB>, worst window <dB> at <frame>, status <equal|first difference at frame k>, PASS|FAIL
```

`snr inf` means the judged stream is bit-exact (no error at all). `<frame>` is the DSP's own frame counter (the
u16 at 0x7ffe of the region the DSP reads), as `capfmt.py` keys on it; where a record's position in the capture
matters (the list below) it is given as `record <n>`, counted from 0. The lines before it:

```
dsp-replay: settled at frame <counter> (record <i>): <i> frames reported, not judged; <k> judged frames had a next frame to compare the status blocks with
dsp-replay: sources enabled in the first captured status that our DSP never started: <source numbers>   (when there are any)
dsp-replay: why: <not settled|snr|window|status, ...>               (only on FAIL)
dsp-replay: window <w>: <frames> frames, snr <dB> (largest difference <d>), ok|BELOW THE LIMIT
dsp-replay: not bit-exact: frame <counter> (record <i>), largest difference <d>     (one line per judged frame that is not)
```

## The method

1. **The DSP.** One `dsp_hle::Dsp` over a 0x80000-byte memory is started the way the application starts it
   (`Initialize` on the audio pipe) and ticked once per captured DSP frame; its state (sources, mixers,
   effects) is carried from frame to frame. Nothing else of the game runs.
2. **Frames.** A frame is a type-1 record and the type-2 and type-3 records after it, up to the next type-1
   record, in record order. (A record's `frame` field is the game's frame counter, not the DSP's: the frames are
   told apart by the type-1 records.) The frame is played when the next type-1 record (or the end) arrives, so
   the next frame's regions are at hand for the status check.
3. **Input.** The two captured regions are copied over the DSP memory's regions (offsets 0x50000 and 0x70000)
   and the DSP ticks; it reads the sources' configurations, the ADPCM table, the DSP configuration and the aux
   buses from the region its own rule picks (the higher frame counter), exactly as it would have in the game.
4. **Sample memory.** A table keyed by (address, size, CRC-32) is filled from the type-3 records that carry bytes
   (the CRC of the bytes is checked). Every type-3 record of a frame, with or without bytes, copies its key's bytes
   into a sparse physical image at its address (4 KiB pages; what was never written reads as zeros, an
   untouched range as an empty span, which `dsp_hle::Source` already treats as zeros) *before* the frame ticks.
   This is what makes a looping PCM16 buffer work: its type-3 record repeats every frame without its bytes once
   its key is known, and the bytes are copied again at that frame. A game that rewrites a buffer in place
   gives a record with a new CRC (and bytes) at the frame it was read again.
5. **Output.** The 160 stereo samples our DSP makes are compared with the frame's type-2 record, sample by sample:
   the energy of the reference, of the difference, and the largest absolute difference are kept per frame.
6. **Status blocks.** What our DSP wrote to the region it did not read (the 24 status blocks of 12 bytes: enabled,
   dirty, sync count, buffer position, current and last buffer id) is compared byte for byte with the same region
   of the next frame's captured regions, which is where the reference's own status landed (that region is the one
   the next frame reads when the counters are consecutive; a gap in the counters skips the comparison for that
   frame, and the number of frames compared is printed).
7. **Settling and the verdict** are below.

### Why it does not depend on timing

The capture holds, per DSP frame, everything the DSP read (both regions, the sample bytes) and everything it
made. The replay feeds our DSP those inputs one frame at a time, so the host's speed, the game clock, the
scheduler and the order of the game's own threads play no part: the same capture replays the same on any machine,
and on any timing model of ours. What is compared is the DSP's own arithmetic, not when the game happened to
write to it. (A native *run*'s own capture is never compared with the reference's PCM: timing models differ and D51
compares their status blocks after a constant shift. That is out of scope here.)

### Settling

A capture starts in the middle of the audio: our DSP starts empty, so a source that was already playing has none
of its state (buffer queue, interpolation history, ADPCM history, filter memory). Only the sources enabled in the
capture's first frame need to settle; a source first enabled later starts from nothing in both and is judged from
its enable frame (it does not hold judging back).

- A source is **in the first frame** when its status block says enabled in the region the DSP reads in the first
  frame, or when our DSP's own first output status says so.
- It is **settled**, counted on our DSP's statuses, at its **second dequeue** since the replay began (the dirty
  flag of the status: a buffer taken from the queue), after **one full loop** of a looping buffer (the position
  falls back with the same buffer id and no dequeue), or when it **stops** (our status says disabled: the source
  was disabled or has no buffer left). A source the capture shows playing that our DSP, starting empty, never
  starts counts as stopped at once; the frames that then differ from the reference are judged and listed, not hidden, and the sources are named in the
  details (`sources enabled in the first captured status that our DSP never started`).
- **Judging starts at the first frame by which every one of them is settled** (the frame at which the last became
  settled is judged). The frames before are reported, not judged.
- **Fewer than 80 % of the capture's frames judged** (first guess, N3.md rule 7) is a `FAIL` with `not settled`
  (so is a source that never settles). For G1's `dsp-1240` (300 DSP frames) that leaves 60 frames for settling.

### The verdict (D75)

- SNR = 10·log10(Σ ref² / Σ (ref − ours)²) over both channels (a reference of energy 0 with any error is −inf; no
  error is `inf`), at least **60 dB** over the whole judged stream and in **every window**: windows of **205 DSP
  frames** (32,800 samples, about one second), tumbling from the first judged frame; a last window of fewer than
  103 frames joins the one before; a capture judged for fewer than 205 frames is one window. (205 and 103, and
  the 60 dB, are D75's; the share of 80 % is a first guess.)
- With `not settled` and no judged frame the verdict line prints `snr n/a, worst window n/a at -`: there is no
  number to print (the tests pin this form).
- A window (or the whole stream) whose **reference RMS is below 1** (Σ ref² under the number of samples) is judged
  by its **largest absolute difference, at most 1**, not by the SNR. D75 states the rule for a window; applying it to
  the whole stream too (a silent capture would otherwise have an SNR of -inf from one LSB) is the worker's reading of
  D75, which the orchestrator will record as a decision.
- **Every source status block equal** over the judged frames: the first difference is printed as `first difference at frame <counter>`.
- **Every judged frame that is not bit-exact is listed**, with its frame and its largest difference. **More than 0
  of them ends the package `needs-decision` with the list** (D75), even when the SNR line says `PASS`: the line
  is the measure of ROADMAP §6, the list is what the maintainer is asked to confirm.

## The numbers of the tests

- `dsp_replay_test`: captures written from our own DSP by a rig that plays the application (it writes the
  sources' configurations into the region the DSP reads next, records both regions at each frame start, the
  output, and the sample reads with their bytes only at a key's first appearance). A 120-frame scenario with four
  sources (a looping PCM16 mono buffer, three queued PCM8 stereo buffers, two ADPCM buffers, a PCM16 stereo source
  that starts at frame 20): settles within the first 20 frames (before the source that starts at frame 20), every judged frame bit-exact, `snr inf`. One output
  sample changed by 300 in record 70 fails (the window that holds it is below 60 dB; the list has that frame
  and the difference 300), a changed status block of the next frame is reported at its frame, queued, looping
  and stopping sources settle where their statuses say, a source that never settles and a capture judged for
  fewer than 80 % of its frames are `not settled`, and the SNR, the windows (205 + 95 is one window of 300;
  205 + 103 are two) and the low-RMS rule are checked against hand-computed values.
- `tests/test_native_dsp_replay.py` (needs `MH4N_AZAHAR_DIR=~/azahar`, clang++ and the built tool): **a capture
  whose type-1 and type-2 records are Azahar's own** (below) replays bit-exact: 200 DSP frames, 194 judged
  (settled at the 7th), 194 bit-exact, `snr inf`, `status equal`, `PASS`; one corrupted output sample gives `FAIL`
  with a window of 45.24 dB and lists exactly that frame; a difference of one least significant bit in
  one sample is listed and the verdict stays `PASS`.

### The second oracle: Azahar's DSP code makes a capture

`tools/native/dsp_oracle/replay_capture.py` (with `replay_driver.cpp` and the scenario `replay_cases.txt`)
compiles Azahar's `source.cpp`, `mixers.cpp`, `effects.cpp`, `filter.cpp`, `codec.cpp` and `interpolate.cpp` as
the N3-02/N3-03 harness does (`MH4N_AZAHAR_DIR`, the pinned commit), plays the scenario on Azahar's own
`GenerateCurrentFrame` as the application would (two sources of each codec, looping and queued buffers, an embedded
buffer, filters, a source that starts later, a game rewriting a looping buffer, a stopped source, the delay effect on the first aux
mix with its bus enabled, 200 frames) and writes the capture the reference runner would:

```
python3 tools/native/dsp_oracle/replay_capture.py --azahar ~/azahar --out build/audio/azahar-two-of-each.mdsp
build/native/bin/mh4n_dsp_replay build/audio/azahar-two-of-each.mdsp
```

What the oracle gives: the two regions at each frame start (the sources' statuses written by Azahar's sources,
the aux buses' intermediate samples, the frame counters) and each frame's 160 samples, all Azahar's own. What it
**cannot** give: the sample reads (type 3): Azahar's `source.cpp` reads through a stand-in memory that cannot
report them, and the files cannot be edited. The scenario names them instead (`read <frame> <address> <bytes>
[every]` lines of `replay_cases.txt`: one read per buffer at the frame its queue is written, and every frame for a
looping PCM16 buffer, as the reference does), and `capfmt.Writer` writes the records with the bytes only the first
time a key appears. A read at the frame a buffer is queued rather than at the frame it is dequeued is harmless for
a replay (memory only changes where the scenario pokes it, and the poke has its own read). The capture holds
synthetic data only.

## Running it on the recordings (the local session)

Nothing here can run in the cloud session (no recordings, no game files): this is the list for the session that
has the dump. Every capture is game data: it lives under `build/` and is never committed; copy counts and dB, not
samples, into notes. Per D75, **any judged frame that is not bit-exact ends the package `needs-decision`** with the
list (the lines `dsp-replay: not bit-exact: frame ...`), whatever the `PASS`/`FAIL` says: stop and send the list.

Build the tool once (Windows: the same, the program is `build\native\bin\mh4n_dsp_replay.exe`):

```
cmake -S native -B build/native -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMH4N_WERROR=ON
cmake --build build/native --target mh4n_dsp_replay_tool
```

1. **G1's recording** (the only golden session whose header has a DSP capture: `# captures: gpu:1500:3, dsp:1240:300`;
   G2, G3, G3-perf, G4, G6 and G7 have none):
   ```
   build/native/bin/mh4n_dsp_replay build/golden/G1/dsp-1240.mdsp --report build/audio/G1-dsp-1240.report.txt
   ```
   Expected (but see *The likely outcome on a real recording* below): exit 0, `dsp-replay: <n> frames judged of 300, <n> bit-exact, snr inf, worst window inf at <frame>, status equal, PASS`
   with `<n>` at least 240, and no `not bit-exact` line (the capture has 300 type-1 and 300 type-2 records and 88
   type-3 records, at most one source enabled; `python3 tools/native/capfmt.py summary build/golden/G1/dsp-1240.mdsp`
   confirms it). On a failure: exit 2 with `truncated`/`not a DSP capture`: the file is damaged, `golden.py check G1`
   says whether its manifest still matches, re-record G1. `not settled` with `n` under 240: send the `settled at` and
   `why` lines and `capfmt.py summary` (the source enabled at the first frame may not have a second dequeue, a loop
   or a stop inside the 300 frames; this is a design question for D75's 80 %, not a bug to tune). `not bit-exact` lines
   or `status first difference`: stop, `needs-decision`, send the whole list and the report file.
2. **The G3 hunt capture**, recorded with the reference runner (about 56 minutes: it runs G3 from the start to
   frame 18000; the save is needed, [`golden.md`](golden.md) *The save*). `14109` is the frame the hunt's quest has
   loaded (`aQuest`, the last of G3's ten scene changes); 3000 DSP frames are about 15 s of audio (about 880 game
   frames, ending near frame 15000, inside the hunt):
   ```
   New-Item -ItemType Directory -Force build/audio
   Copy-Item -Recurse build/golden/G3/user-before build/audio/G3-hunt-user
   $env:TZ = "UTC0"
   <the N0-29 reference runner> baserom/juego.cxi --install baserom/update_v130.cia --user build/audio/G3-hunt-user --new-3ds off --cpu recomp --code <the reference code library> --input-file tools/native/golden/G3.txt --frames 18000 --capture-dsp 14109:3000:build/audio/G3-hunt.mdsp
   python3 tools/native/capfmt.py summary build/audio/G3-hunt.mdsp
   build/native/bin/mh4n_dsp_replay build/audio/G3-hunt.mdsp --report build/audio/G3-hunt.report.txt
   ```
   (the runner and the library are the ones of `build/golden/G3/manifest.json`: `runner.path` and `code_library.path`; on Linux `export TZ=UTC0`
   and `cp -r`). Expected: `capfmt.py summary` says 3000 `dsp_frames`, `pcm_non_silent_frames` most of them and
   `sources_enabled_max` several; then exit 0 and `dsp-replay: <n> frames judged of 3000, <n> bit-exact, snr inf, worst window inf at <frame>, status equal, PASS`
   with `<n>` at least 2400 and no `not bit-exact` line. The same likely outcome as for G1 applies (*The likely outcome on a real recording*): sources already playing at frame 14109 give record 0, 0 bit-exact, FAIL, and `needs-decision`. If `capfmt.py summary` shows a silent capture (the hunt's audio
   starts later than 14109), record again with a later start (G3's hunt is in view from about 16740) and say which.
   On a failure: as for G1; `not settled` with many sources is the likely first finding (a looping source with a very
   long buffer, or one that is only ever played once, never reaches its second dequeue): send the `why` and `settled`
   lines and the sources named in `not settled: source ...` (they are source numbers, not data).
3. **Every other golden session with a DSP capture:** none (checked in the headers of `tools/native/golden/*.txt` and
   in `golden.md`; G2, G3, G3-perf, G4, G6, G7 record no DSP capture, and G3-perf and G4 only the GPU). A session
   recorded later with `# captures: dsp:...` is replayed the same way with its `build/golden/<S>/dsp-<start>.mdsp`.

### The likely outcome on a real recording

A source that was already playing when the capture starts has no enable or dirty flags in the captured
configuration (the game set them long before): our DSP, starting empty, never plays it, it counts as stopped at once
(above), judging starts at record 0 and every frame that has such a source differs. The output is then
`settled at frame <n> (record 0)`, `0 bit-exact`, every frame listed, `FAIL`, and the line `sources enabled in the
first captured status that our DSP never started: <numbers>`. G1's `dsp-1240` begins about 6 game frames after its
sound starts, so this may well be what it does. That outcome means **"our DSP never started a source the capture
shows playing"**: it is a design question of D75 for the maintainer (how a replay is to get a source's state when the
capture starts in the middle of it), **not a DSP bug**. The local session reports `needs-decision` with the numbers
(the verdict line, the source numbers, the count of listed frames) and retunes nothing.

Put in the notes only the three result lines, the counts of `capfmt.py summary`, and the list if there is one.
