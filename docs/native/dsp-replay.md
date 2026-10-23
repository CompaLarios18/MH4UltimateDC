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

## Results (dump-check of 2026-10-23)

Numbers only. The tool is `mh4n_dsp_replay` built from the N3-04 branch (RelWithDebInfo, `MH4N_WERROR=ON`, Windows,
llvm-mingw clang). The captures stay under `build/` and are not committed.

### Step 1: G1's `dsp-1240` (no emulator)

`capfmt.py summary`: 300 DSP frames (counters 4434 to 4733), 300 type-1 and type-2 records, 88 type-3 records
(88 unique, 0 without bytes), `pcm_non_silent_frames` 300 of 300, `sources_enabled_max` 1.

```
exit 1
dsp-replay: settled at frame 4434 (record 0): 0 frames reported, not judged; 299 judged frames had a next frame to compare the status blocks with
dsp-replay: sources enabled in the first captured status that our DSP never started: 0
dsp-replay: why: snr, window, status
dsp-replay: window 0: 300 frames, snr 0.00 (largest difference 6067), BELOW THE LIMIT
dsp-replay: 300 frames judged of 300, 0 bit-exact, snr 0.00, worst window 0.00 at 4434, status first difference at frame 4434, FAIL
```

- Frames listed as `not bit-exact`: 300 of 300 (frame 4434 with largest difference 2486, up to 5611 at frame 4438;
  frame 4733 with 4657).
- The line naming sources our DSP never started names source number 0 (a source number, not a count; the
  shape in *The likely outcome on a real recording*, a non-empty list, is what the run printed; see
  *Diagnosis*); the first status block differs at frame 4434.
- Per D75, a judged frame that is not bit-exact ends the package `needs-decision`; nothing was retuned and no code
  changed.

### Step 2: the G3 hunt capture (`--capture-dsp 14109:3000`, reference runner of N0-29)

The runner ran G3 to frame 18000 and exited 0 (about 56 minutes). `capfmt.py summary`: 3000 DSP frames (counters 48745
to 51744, game frames 14109 to 14966), 3000 type-1 and type-2 records, 316 type-3 records (233 unique, 83 without
bytes), `pcm_non_silent_frames` 2454 of 3000, `sources_enabled_max` 20.

```
exit 1
dsp-replay: settled at frame 48745 (record 0): 0 frames reported, not judged; 2999 judged frames had a next frame to compare the status blocks with
dsp-replay: why: snr, window
dsp-replay: window 0: 205 frames, snr inf (reference RMS below 1: judged by the largest difference 0), ok
dsp-replay: window 1: 205 frames, snr inf (reference RMS below 1: judged by the largest difference 0), ok
dsp-replay: windows 2 to 14: snr between -0.02 and 0.07 (largest differences 9964 to 23330), BELOW THE LIMIT
dsp-replay: 3000 frames judged of 3000, 546 bit-exact, snr 0.01, worst window -0.02 at 49770, status equal, FAIL
```

- Frames listed as `not bit-exact`: 2454 of 3000 (the same count as the non-silent frames); the 546 bit-exact
  frames are the silent ones.
- The line naming sources our DSP never started is not printed; the status blocks are equal over all judged frames.
- Per D75 the package ends `needs-decision`; nothing was retuned and no code changed.

## Diagnosis (2026-10-23)

Numbers and neutral words only; the captures and the scratch programs stay under `build/` (git-ignored) and are
not committed. Question: why the real recordings do not replay (G1: 0 of 300 frames bit-exact, 0.00 dB; the G3
hunt capture: 546 of 3000, 0.01 dB), when N3-02 and N3-03 reproduce Azahar's code on 121 + 155 synthetic cases and
a capture written from Azahar's code replays bit-exact.

**Answer.** Neither our DSP nor the replay harness is at fault. The captures do not hold the DSP's state from
before they start, and Azahar's own code, given the captured inputs the same way, fails against the recordings
exactly as ours does. For the G3 hunt capture the missing input is one value, the master volume of the mixers: with
its dirty bit set in the first captured frame the replay of the recording is bit-exact over all 3000 frames. G1's
capture starts with a source already playing; that state cannot be rebuilt from the capture, but a capture of G1
that starts at the first DSP frame replays bit-exact (4900 of 4900 frames, the tool unchanged).

### What was run

The second oracle, inverted (scratch programs in `build/n3-04`):

- `mk_frames.py` turns a capture into one input file (per frame: both regions, the reference PCM, the sample
  reads with their bytes, the bytes of a read without bytes taken from its earlier (address, size, CRC) key as the
  replay does; no key was missing).
- `real_driver` is Azahar's own frame code at the pinned commit `5f3306d` (`source.cpp`, `mixers.cpp`, `effects.cpp`,
  `filter.cpp`, `codec.cpp`, `interpolate.cpp` through the oracle's `driver_frame.cpp` and stand-ins, `-O1
  -ffp-contract=off -fwrapv`; the stand-in memory widened from 4 MiB to the New 3DS's whole 256 MiB of FCRAM, since
  the recordings read at 24.6 MiB (G1) and 29.9 MiB (G3) above the start of FCRAM) playing that file: both captured
  regions copied over its DSP memory before each frame, the sample bytes placed at their addresses,
  `GenerateCurrentFrame`.
- `ours_driver` is our `dsp_hle` library (the build of `mh4n_dsp_replay`) fed the same file the same way.
- `cmp.py` compares, per frame, the reference PCM, Azahar's, ours, both DSPs' status blocks and intermediate mix
  samples, and each DSP's statuses against the next captured frame's.

### Numbers

| | G1 `dsp-1240` (300 frames) | G3 hunt (3000 frames) |
|---|---|---|
| reference vs Azahar's code | 0 bit-exact, 0.00 dB | 546 bit-exact (all silent), 0.01 dB |
| reference vs our DSP | 0 bit-exact, 0.00 dB | 546 bit-exact, 0.01 dB |
| Azahar's code vs our DSP: PCM / statuses / intermediate mix equal | 300 / 300 / 300 | 3000 / 3000 / 3000 |
| statuses vs the reference's next frame, Azahar's code and ours | 0 of 299 equal | 2999 of 2999 equal |
| `mh4n_dsp_replay` on the capture | 0 bit-exact, 0.00 dB | 546 bit-exact, 0.01 dB |

So Azahar's code does not reproduce the recordings from the captured inputs either, and our DSP agrees with it to
the bit on every frame of both captures (PCM, statuses, mix buffers), with or without the change below. The replay
tool's numbers are the same as the independent feed's, so it feeds the DSP as the game does (no harness bug).

### G3: the master volume

- In Azahar's mixers the master volume is state: `intermediate_mixer_volume` starts at 0 and takes the
  configuration's value only in a frame whose `master_volume_dirty` bit is set. The captured DSP configuration
  holds the value (1.0 in one region of the pair, 0.0 in the other, 1500 frames each) but the dirty bit is set in
  **0 of 3000** captured frames (G1: 0 of 300): the game set it when it started the DSP, long before the capture.
  The configuration's dirty word in the captured frames holds the headphones bit (bit 28) in every frame and the
  aux front bypass, aux bus enable and aux return bits (6, 8, 24) in 2, 1 and 2 frames of G3. A DSP that starts
  empty at the first captured frame therefore mixes at volume 0: its output is silent where the reference's is not.
- The first frame whose reference output is not silent is record 546 (counter 49291; sources 0 and 1 are enabled,
  with their dirty bits, in record 544); both DSPs make 0 there and in every later frame. The 546 bit-exact frames
  are the ones before it.
- The test: the same capture with the master-volume dirty bit (bit 16 of the configuration's dirty word) set in the
  first record, in the region the DSP reads, and nothing else changed. Azahar's code: **3000 of 3000** bit-exact,
  SNR inf. Our DSP: 3000 of 3000. `mh4n_dsp_replay` on that copy: `3000 frames judged of 3000, 3000 bit-exact, snr
  inf, worst window inf at 49155, status equal, PASS`, exit 0, no listed frame. With the output-format bit alone: 546
  bit-exact (as unchanged); with the master volume alone: 3000; master volume with the output format, and with
  all the mixer bits (aux enables, aux returns, output format): 3000.
- The sample inputs of G3 are complete: 316 sample reads, 83 without bytes, every one of the 83 repeats a key given
  by an earlier record (233 unique), and with the master volume applied the whole stream is bit-exact, so no frame
  depends on bytes the capture lacks. No source is enabled in the first captured status; the sources start inside
  the capture and their statuses already matched in every frame without the change.

### G1: a source that was already playing

- The line `sources enabled in the first captured status that our DSP never started: 0` names **source number 0**
  (the Results above read it as a count). That source is enabled and playing in the first captured status (second DSP
  frame of a queued buffer, status dirty flag clear); the capture's first sample read is at record 1, so the buffer
  playing at record 0 was started before the capture and its bytes are not in it. Our DSP, starting empty, leaves the
  source disabled: its status block differs from the reference's in record 0 (frame 4434) and in 299 of 299
  compared frames. This is the case D88 describes; the mixers' master volume is missing in G1 as in G3.
- Best effort to feed that state in (scratch only, not a method): the mixer bits of G3, the source's configuration
  from the region with the enable flag, its current buffer as the embedded buffer at the status position (bytes
  unknown: zeros), the queued ids above it in the four slots, and one synthetic frame before record 0 so that the
  frame in which a source dequeues (it makes no output) falls outside the judged ones. Result: still 0 of 300
  bit-exact; the median per-frame SNR over records 10 to 299 is 12.9 dB (3.3 to 21.1); the start position moved by +2
  samples gives 32.0 dB (21.7 to 39.9), by -3 gives 9.2 dB. Our DSP and Azahar's code agree on all 300 frames in
  every variant. What is lost is the interpolation phase and history (the status gives only whole samples), the bytes
  of the buffer playing at the start, and the gain and filter state; none of it is in a capture that starts
  mid-play.

### Confirmed: a capture from the first DSP frame replays bit-exact, with nothing changed

G1 run again with the reference runner of N0-29 (the manifest's runner and code library, `TZ=UTC0`, the same input
file, `--frames 1450`; 42 s of runner time, started after a wait of about 13 minutes for the emulator lock) with
`--capture-dsp 0:4900:FILE`, so that the capture starts at the first DSP frame. `capfmt.py summary`: 4900 DSP frames
(counters 0 to 4903, game frames 0 to 1308; the only gap in the counters is between the first two records, 0 and 5,
and that pair is not compared for statuses), 151 sample reads (148 unique, 3 without bytes), `pcm_non_silent_frames` 909 (records 1033 to
4899), `sources_enabled_max` 1, 325 MB. The 300 frames of `dsp-1240` are in it (found by counter): all 300 PCM
records are identical to the old capture's, so the reference run is deterministic. `mh4n_dsp_replay` on it,
unmodified, no priming, no copy:

```
dsp-replay: settled at frame 0 (record 0): 0 frames reported, not judged; 4898 judged frames had a next frame to compare the status blocks with
dsp-replay: 4900 frames judged of 4900, 4900 bit-exact, snr inf, worst window inf at 1029, status equal, PASS
```

exit 0, no listed frame. This is the first real recording that replays: our DSP, started the way the application
starts it, reproduces the reference runner's mix and its source statuses to the bit over 4900 frames, 909 of them
with sound. The capture is under `build/n3-04/` (game data, not committed).

### The quick suspicions, in the order given

1. *Order of judging.* G3 has no source enabled in the first status, so no source holds judging back: judging starts
   at record 0 and the silent windows 0 and 1 pass; correct under D88. Not a cause.
2. *First frame mid-play.* G1: yes, source 0 (above). G3: no source is playing, but the mixers are already
   configured (the master volume above). The settling rule only knows sources; the mixers' state is the part it
   misses.
3. *Sample rate and tick alignment.* The frame counters of both captures are consecutive (0 gaps); the tick field
   is 1,310,720 plus a few ticks of jitter and plays no part in a replay. The statuses of G3 match in 2999 of 2999 frames,
   which they could not if a frame were skipped or doubled. Not a cause.
4. *Status blocks.* G3's are equal throughout (both DSPs, 2999 of 2999). G1's differ from record 0 (frame 4434) because
   source 0 is disabled in ours (not started) and enabled, playing, in the reference's.

### Recommended next step

Not a DSP change (`native/src/dsp_hle` agrees with Azahar's code on all 3300 captured frames, on the real inputs
as on the synthetic ones) and not a threshold change. D75 asks the DSP to reproduce a recording; a recording that
starts after the DSP was configured must carry, or be given, that configuration. In order of cost:

1. **Record from the first DSP frame** (`--capture-dsp 0:N:FILE`, which starts at the first frame at or after game
   frame 0): every dirty flag the game ever sets is then in the capture and the replay needs no priming. For G1,
   `dsp-1240` could be replaced by a capture of the first 4750 DSP frames or so (the last 300 are today's; about 310
   MB at 64.6 KiB a frame). **Done for G1 above: 4900 of 4900 bit-exact, PASS.** For the G3 hunt it would be
   51,744 frames, about 3.4 GB and the 56 minutes of the run: the next option is cheaper.
2. **Give the replay the state the capture lacks.** Either a record of the mixers' and sources' state at the first
   captured frame (a hook into Azahar's `Mixers` and `Source` state in `port/azahar/azahar.patch` and a new record
   type in the capture format: N0-06 and N0-29's files), or, cheaper, replay-side priming of the **mixers only**:
   apply the DSP configuration of the first captured frame's read region that holds the non-zero master volume (G3's
   is the first one; G1's first record holds 0.0 there and its 1.0 in the other region, so the rule has to look at
   both: D92) as if its mixer dirty bits (aux enables, master volume, aux returns, output format) were set. The evidence above is that this alone makes the G3 hunt capture replay
   bit-exact; it cannot make G1's playing source exact (the phase, the history and the unseen bytes), so G1 needs
   option 1 or a capture that starts before its source does.
3. D75's settling rule and D88 describe sources only. If option 2 is taken, the verdict needs one more sentence: the
   first captured frame's mixer configuration is applied as written (it is the game's own value, not a tuned one),
   and which region holds it must be decided (here the master volume is in one region of the pair and 0.0 in the
   other, in every frame).

Which of them is a decision for the orchestrator and the maintainer: option 1 changes how recordings are made,
option 2 changes what the replay may assume about the DSP before its first frame.