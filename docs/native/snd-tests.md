# Our own DSP test programs (N3-01)

Three 3DS programs of our own drive `dsp::DSP` as the SDK does, with samples they make themselves, and
print what the DSP did: the status of the sources after every audio frame and a CRC of the final mix.
The output of the reference runner (Azahar's HLE DSP) is committed in `port/tests/expected/snd/`; it is
the oracle for "decoding and interpolation bit-identical to Azahar's" and for the mix, on public CI
without the game: the native DSP (N3-02, N3-03) is held to it by N3-11. There is nothing of the game in
the programs, the expected files or the tests: the samples come from a 16-bit LCG and an integer
recurrence for a sine, and the floats of the configuration are written as their bit patterns (the
programs use no floating point).

| File | What |
|---|---|
| `port/tests/dsp.h` | the application side of the DSP: starting it, the frame loop, accessors for the configuration |
| `port/tests/snd_pcm.c` | one PCM source at a time: formats, rates, interpolation, loops, queues |
| `port/tests/snd_adpcm.c` | one ADPCM source at a time, partial updates of the embedded buffer |
| `port/tests/snd_mix.c` | three looping sources through the mixers, the aux buses, the effects and the filters |
| `port/tests/expected/snd/<prog>.txt` | Azahar's output |
| `tests/test_native_snd.py` | the tests; imports the helpers of `tests/test_conformance.py` |

The programs are never in `PROGRAMS` of `tests/test_conformance.py` (that file is the N1 packages'), and
are not named `conf_*`; `tests/test_conformance.py -k every_port_test_program` still builds them, since
it builds every `port/tests/*.c` that has a `_start`.

## How the programs drive the DSP (`dsp.h`)

1. `srv:` gives `dsp::DSP`; `LoadComponent` takes a 0x400-byte zero component (Azahar's HLE does not run
   it); `RegisterInterruptEvents` (interrupt type Pipe, channel Audio) registers an event;
   `WriteProcessPipe` writes the audio pipe's initialise command, which makes the DSP answer on the pipe
   and raise the event.
2. `ReadPipeIfPossible` reads the number of structures (15) and their DSP addresses;
   `ConvertProcessAddressFromDspDram` turns the addresses of the frame counter, the source
   configurations and statuses, the ADPCM coefficients, the DSP configuration and status, the final
   samples and the intermediate mix samples into process addresses, for region 0 and, with the address
   or-ed with 0x10000, region 1. The program checks them against the layout it expects (`FAIL structure
   address <i>` otherwise).
3. Sample buffers are in the linear heap (`svcControlMemory`); the physical address is the heap offset
   plus 0x20000000 (FCRAM). `FlushDataCache` follows what the program wrote.
4. The frame loop: wait for the audio pipe's interrupt, which the DSP raises after generating a frame.
   The DSP read the region with the higher frame counter (a tie reads region 1) and wrote the other
   one: that other region now holds the frame's source statuses and final samples, and it is also the
   region the DSP reads next. The program records its status and the CRC of its final samples, then
   changes its configuration, and last raises its frame counter above the other region's. Every
   configuration is therefore written before the frame that reads it, and all in one go, right after the
   interrupt: a change goes through a dirty bit of the region about to be read (the DSP clears the bits
   it reads, so every field is written in the frame that uses it; nothing relies on what an earlier
   frame left in the region).
5. **Nothing is printed while the DSP runs.** The lines are collected and printed at the end, so the time
   the program spends is a few thousand instructions per frame. The program also compares the system
   tick at each interrupt with the previous one: more than one and a half audio frames (1,310,720 ticks
   each) apart prints `missed <n>`, a failure line (on the reference runner the distance is exactly one
   frame, every time). The DSP has no frame counter the program can read; the interrupt event is
   one-shot, so a frame the program is too late for cannot be counted any other way.
6. `dsp_finish` writes the shutdown command, waits for `RecvData` to say the DSP is off, prints the
   lines and ends the process.

## Output

```
<prog>: start
f<k>: s<i> en=<0|1> pos=<n> buf=<id> last=<id> sync=<n>      one for each watched source
f<k>: mix crc=<8 hex digits> peak=<n>
<prog>: done
```

`k` counts from 0 at the first frame in which source 0 reports itself enabled. A watched source is one
the program has started (it keeps its line after it has run out of buffers: `en=0`, `last=<id>`). `pos`
is `buffer_position`, `buf` the current buffer id, `last` the last buffer id, `sync` the sync count the
DSP reports; the "buffer changed" flag (`current_buffer_id_dirty`) is part of the status block but not of
the line (the format is the work package's). `crc` is the CRC-32 of the 160 stereo `s16` of the final
samples as the DSP wrote them (little-endian, as the bytes lie in memory) and `peak` their largest
absolute value. A line `FAIL <what> <hex>` or `missed <n>` is a failure; the expected files have none.

The test requires at most 400 frames and at least 300 frame lines (lines that start with `f<k>:`) in an
expected file; the files have `snd_pcm` 344, `snd_adpcm` 305 and `snd_mix` 318 frames. Each program's
source is under 300 lines (the limit of the `conf_*` programs): "under 300 lines" is read as the
program's source, as for those, since the output holds well over 300 lines.

The programs also print `missed <n>` (a failure) when the system tick shows a frame interrupt more than
1.5 audio frames after the previous one, or when the program is a whole audio frame or more late in
publishing its configuration after an interrupt.

## What the programs play

**Things Azahar does that show in the expected output**, so a native DSP that "fixes" them fails:

- A source that was just enabled dequeues its first buffer and produces nothing in that frame (frame 0 of
  every case is silent, `pos=0`); the position then runs 159, 319, ... because the interpolation keeps
  samples back.
- Polyphase interpolation is Azahar's linear (`source.cpp`: "TODO(merry): Implement polyphase
  interpolation"); at the rate 1.0 the fraction is always 0 and none and linear coincide.
- Buffers wait in a queue ordered by buffer id, the embedded buffer included: a lower id queued in
  the same frame plays first.
- Partial updates of the embedded buffer are honoured for PCM16 only; for ADPCM they are ignored. A
  PCM16 partial update that sets `is_looping` does not make the buffer loop: the source only records
  the flag in its current-buffer state and never queues the buffer again, so the buffer ends at its
  new length (snd_adpcm case 32). A partial update that shortens the buffer below the play position
  resets the position to 0 and keeps the whole buffer (case 33).
- The buffer's own predictor/scale field of an ADPCM buffer is never read (the data's frame headers
  decide); the history `adpcm_yn` is taken only when the buffer's dirty flag is set, including each time
  a looping buffer is dequeued again. The data of the programs start at a frame whose predictor selects
  a non-zero coefficient pair, so the history is heard: cases 17 and 18 differ, and so do the third
  buffer of case 21, the loop of case 25 and the three queued buffers of case 35 when the history or
  its flag is dropped.
- A source that is disabled loses its filter state *and its coefficients*: `SourceFilters::Enable`
  resets a filter that is turned off to passthrough, so a filter is configured again when it is turned
  on again (snd_mix, interrupt 234).
- A looping PCM16 buffer is read again from memory every frame for the part not yet played, so a program
  that rewrites it while it plays is heard at once (snd_pcm case 54).
- The master volume starts at 0 and the aux return volumes too: nothing is heard until the program
  sets them.

**`snd_pcm`** - source 0, gain 1.0 to the main mix and 0 elsewhere; each case resets the source and sets
its sync count to the case number `n` (1-based); a case ends when the source stops (looping cases after
ten frames). 54 cases:

| Cases | What |
|---|---|
| 1-12 | PCM16 mono, 480 samples, rates 1.0, 0.5, 1.37, 2.9; case `1 + 3 * rate + mode`, mode none, linear, polyphase |
| 13-18 | PCM16 stereo at the four rates (linear), then none and polyphase at 1.37 |
| 19-24 | PCM8 mono: the four rates (linear), none at 1.37, polyphase at 0.5 |
| 25-28 | looping buffers (PCM16 mono, PCM16 stereo, PCM8, and a loop that gets a one-shot buffer with a lower id) |
| 29-33 | queues: embedded 5 with queued 9 and 2 in one frame; queued 8 and 4 while the embedded buffer (its id is the case number, 30) plays; four queued buffers and no embedded one (ids 7 3 12 1), twice (200 samples, and 40 samples, which gives several dequeues in one frame); a partial reset that drops two queued buffers |
| 34-41 | a play position; rate changed while playing; sync count changed; disable and enable; gain changed (the ramp); rate 0 (Azahar uses 1.0); an unmapped address; an address with its low bits set (masked) |
| 42-47 | buffers of 1, 2, 3, 5, 4 (stereo) and 0 samples |
| 48-53 | rates 0.25 and 2.0, PCM8 stereo |
| 54 | a looping PCM16 buffer of 640 samples whose memory the program rewrites after two frames |

**`snd_adpcm`** - source 0, ADPCM mono unless said; the coefficients are two sets of eight stable
predictor pairs the program sets, the data two sets of 80 frames (the predictor and scale of each frame
cycle, data A from predictor 1; set B is louder and clips). 38 cases:

| Cases | What |
|---|---|
| 1-12 | data A, 1120 samples, the four rates, each in the three modes |
| 13-16 | data B, coefficient set B, four rates |
| 17-20 | history given with the dirty flag, given without it (ignored), no coefficients, set B on data A |
| 21-22 | a second and third buffer queued (the history carries over, then is given again); coefficients changed between two buffers |
| 23-25 | looping buffers (one with a history that is reloaded at every loop) |
| 26-29 | lengths 13, 15, 27 and 1 (not a whole number of 14-sample frames) |
| 30-33 | partial update of the embedded buffer: ignored (ADPCM); extends a PCM16 buffer; extends one and sets looping (it does not loop); shortens one below the play position |
| 34-35 | a play position; three queued buffers with no embedded one |
| 36-38 | rate 0.25 and 2.0 |

**`snd_mix`** - source 0 a stereo tone, 1 and 2 mono tones, all looping; every step is a row of the
table `ev` in the source (the interrupt number, the operation). Interrupts 0-318:

| From | What |
|---|---|
| 0 | master volume 1.0, output format stereo (the SDK's default), source 0 (gain 1.0) into the main mix |
| 12-42 | master volume 0.5, 0.25, 0, 1.0 |
| 48-84 | source 1 into the main mix: both channels at 0.25, then 0.5 left only, 0.5 right only, 0.25 with the rear channels |
| 96-146 | source 2, with gain 0.125 in the main mix and 0.5 into aux bus 0 at once (so three sources are in the main mix, one of them also in a bus): bus 0 inside the DSP; bus 0 through the application's memory (the DSP reads back last frame's send; the program halves it for eight frames); bus 1 (gain 0.5) read back unchanged |
| 156-204 | the delay effect on bus 0 (2 frames, g 0.5, a 0.75, b 0.25) and on bus 1 (1 frame, a 1.0) |
| 210-258 | the simple filter on source 0; the biquad (turning the simple filter off resets it); both (the simple filter is configured again at 234); source 1's simple filter |
| 264-288 | output format mono, surround, stereo |
| 294-318 | master volume 2.0 (clipping), then the sources stop |

## Running

```
python3 -m pytest -rs tests/test_native_snd.py                  # no runner: the builds and the checks of the files
MH4U_RUNNER=<run.sh of the reference> MH4N_SND_REQUIRE=reference python3 -m pytest -rs tests/test_native_snd.py
MH4N_RUNNER=<mh4u_native_runner> MH4N_SND_REQUIRE=native[,pcm] python3 -m pytest -rs tests/test_native_snd.py
```

- Always: each program builds, recompiles and links into the paged, native and deterministic native code
  libraries; the expected files are well formed (at least 300 frame lines, at most 400 frames); `snd_pcm` shows the modes
  (none differs from linear at the rates that are not 1.0, polyphase equals linear).
- `MH4U_RUNNER` (the reference runner: `--cpu jit --new-3ds off --ms 5000 --user <fresh>`): the output
  is the expected file, in two runs.
- `MH4N_RUNNER` (the native runner, with `--deterministic` and the deterministic native library, N1-25):
  the `s<i>` lines are the expected file's; the `mix` lines too when `MH4N_SND_REQUIRE` has `pcm`
  (N3-11 sets it once the native DSP mixes). A `FAIL` or `missed` line is a failure.
- `MH4N_SND_REQUIRE`, comma-separated, `reference`, `native`, `pcm`: a runner it names that is not set
  fails its tests; one it does not name is skipped (`MH4N_RUNNER not set`).

To rewrite the expected files, from two identical runs of the reference runner (a program whose two runs
differ, or whose output is not well formed, is not written):

```
MH4U_RUNNER=<run.sh of the reference> python3 tests/test_native_snd.py --regenerate [snd_pcm snd_adpcm snd_mix]
```

The reference runner is the Azahar-based `mh4u_runner` of `native.yml`'s artifact (`docs/native/golden.md`);
it repeats exactly because it does its file reads in place and starts the emulated clock at a fixed date
(`port/README.md`, *Determinism*), and the DSP's frames are events of the emulated clock. Each program
was run ten times, three at once, without a difference.
