# The audio output stream (N3-05)

What sits between the native DSP and an audio device, with no SDL: `native/src/audio_out/`
(`mh4n_audio_out`). The DSP runs on the scheduler thread and makes sound in step with the game
clock; the device runs on a thread of its own and wants samples at its own pace. The stream
between them never makes the game wait (D63). The SDL3 device is N3-06
(`native/app/audio/`); this document gets its section when that package lands.

Every number marked **first guess** (N3.md, rule 7) is one: the maintainer's runs (N3-14, N3-27)
confirm it, and no worker retunes it. A wrong one stops its package with a `needs-decision`
table.

## The clock (D63)

The game clock drives the DSP: 160 sample frames every 1,310,720 ticks, `kTicksPerSecond / 8192` =
32,728.498046875 Hz of game time (`audio_out::kDspRate`, held as the exact fraction
`268111856 / 8192`; `kStreamSampleRate` = 32,728 is the integer rate the WAV header and the device
format use). The audio device never paces the game: when the game clock stands still (pause,
`Hold`, a stall) the DSP makes nothing, the stream underruns, and the device plays the fade and
silence. Refresh-rate changes (D44) do not touch the DSP.

## The stream: `--audio stream`

`audio_out::StreamSink` is a single-producer single-consumer ring of stereo `s16` frames, 8192 frames
(about 250 ms, `kRingFrames`). It is the `AudioSink` the runtime owns; `audio_out::Find(Runtime&)`
gives the app the same stream (a `shared_ptr`, so the device can hold it for as long as it likes).

| Call | Thread | What it does |
|---|---|---|
| `Queue(stereo, frames)` | the scheduler | copies the frames into the ring and returns; no lock, no allocation, no wait, no logging |
| `Pull(out, frames)` | the device's | copies what there is and fills the rest by the underrun rule; returns how many frames were real; no lock, no logging |
| `Fill()`, `Stats()`, `Target()`, `Priming()` | any | lock-free reads |
| `SetTarget(frames)` | any | the fill the consumer waits for (set at creation from `--audio-latency`) |

**Overflow** (first guess: a fill above 200 ms, `kOverflowFrames` = 6546 frames). `Queue` never moves
the consumer's read index. When the frames it just wrote leave more than the mark waiting, it counts
an overflow and raises a "read at least from here" index (`min_read_`) so that only the newest
200 ms stay. `Pull`, at its next call, skips the older frames first and adds them to `dropped`.
`Fill()` already leaves the marked frames out.

**A full ring** (nobody pulls, or the consumer stopped). `Queue` writes only what fits, `8192 - (written - read)`
frames, discards the rest and counts it in `dropped`. With no consumer at all (a device that failed to
open, or `--audio stream` in the runner) the ring therefore stays full, `dropped` grows by what the DSP
makes, and `Queue` never blocks. A device that opens late finds the oldest frames already gone
(`Fill()` is 200 ms) and starts there.

**Underrun** (first guess: 64 samples). A `Pull` that cannot be filled gives the real frames it has, then
a linear fade to zero from the last sample given over 64 frames (`last * (64 - k) / 64`, rounded toward
zero, per channel), then zeros. The fade runs across pulls if they are shorter than 64 frames. One
underrun is counted per run of short pulls, and only once a real frame has been played: a device that
opens before the game has made any sound has nothing to interrupt.

**Priming.** At the start, and after every underrun, `Pull` gives silence (the fade, then zeros) and takes
nothing from the ring until it holds `max(target, block)` frames (the block is the `frames` argument,
and the whole is capped at the overflow mark). So when the game clock moves again the stream fills to
the target before anything plays, and before the controller steers: `Priming()` is true meanwhile, and
the consumer does not call the controller (it calls `Reset()`). Playing resumes with the first real
frame at full level, with no fade-in (the spec only has the fade-out).

`Stats()`: `queued` (frames accepted into the ring), `pulled` (real frames `Pull` gave), `underruns`,
`overflows`, `dropped` (frames skipped by `Pull` after an overflow, plus frames `Queue` discarded on a
full ring). Offered frames = `pulled + dropped + Fill()`, once `Pull` has applied the last skip.

`audio_out::PublishFrame(game_frame)` and `LastFrame()` carry the newest game frame for the audio log
(N3-06's `--audio-log`): `stream` subscribes to `Runtime::OnFrame` and publishes it. The value is one per
process.

## The drift controller

`audio_out::DriftController` (pure, `drift_controller.h`) is the formula of Azahar's time stretcher
(`src/audio_core/time_stretch.cpp`, `Process`) without SoundTouch, with its output bounded.
`Update(fill, target, dt)` returns the factor by which the consumer multiplies its resampling ratio
(frames of the stream used per frame played): above 1 it uses the stream faster, so the fill goes
down.

```
fullness   = fill / (2 * target)                     Azahar's backlog_fullness, whose target is 0.5
tweak      = (fullness - 0.5) * (dt / 0.050)         the tweak over 50 ms
ratio      = (1 + 2 * tweak) ^ (tweak < 0 ? 3 : 1)   a fill below the target is corrected harder
factor    += (1 - exp(-dt / 0.712)) * (ratio - factor)   the low-pass
factor     = clamp(factor, 0.995, 1.005)             +-0.5 %, first guess
```

Azahar's maximum backlog is twice its target (0.5 of 250 ms); that is where `2 * target` comes from. Two
details are ours, not Azahar's: the ratio is clamped before the low-pass as well as after (a long time step
cannot wind the state up), and a base that is not positive (a time step so long that the tweak passes
-0.5) counts as the lowest factor. A call with a time step or target that is not positive, or a fill that is
not a number, changes nothing.

**How a consumer steers (what N3-06 builds).** Before each device block, with `need = block * kDspRate /
device_rate`: if `stream.Priming()`, call `Reset()` and use 1; otherwise `factor = Update(Fill() - need / 2,
Target(), seconds since the previous call)`, then ask the stream for `need * factor` frames and give them to the
resampler. The fill is read in the middle of the block, not before it: the block itself moves the fill by
`need` frames (a quarter of the 40 ms target for a 10 ms block), and steering on the value before the pull
would hold the fill after the pull a quarter of the target too low. The tests drive the controller this way.

Two facts N3-06 has to plan for:
- **A late or resuming device starts at the mark.** A device that opens late, or that resumes after a
  pause while the DSP kept queueing, finds up to 200 ms of old audio (the ring keeps the newest
  `kOverflowFrames`), and starts with the fill at the mark; the controller needs about 33 s to bring it back
  to the target. So open the device early (before the game makes sound) and keep it pulling, or discard on
  resume (pull and throw away the excess) instead of playing the old audio.
- **A pull larger than the mark can never be satisfied.** Priming waits for `max(target, block)` frames
  capped at `kOverflowFrames` (6546 stream frames), and an overflow keeps the fill at or below it, so a
  single `Pull` of more than 6546 frames always comes back short. Keep the device's blocks (after the
  ratio) well below that, 4096 device frames at 48,000 Hz is about 2800 stream frames and fine.

`Find(rt)` stops finding the stream when the run ends (`Runtime::OnFinish`); a pointer taken before stays
valid.

## `--audio wav`

`--audio wav --audio-file FILE` writes every queued frame to a 16-bit stereo PCM WAV at 32,728 Hz. The header
is written with zero sizes at the start and again with the real ones when the run ends (`Runtime::OnFinish`;
the file's destructor does it too). A file that cannot be created ends the run with status 2, and so does
`--audio wav` without `--audio-file`. It is a test sink: `Queue` writes to the file on the scheduler thread.

## Options (runner scope)

| Option | Meaning |
|---|---|
| `--audio stream` | the stream above (`--audio` itself is the runtime's; the default is `null`) |
| `--audio wav` | the WAV sink |
| `--audio-latency MS` | with `stream`: the target fill, 10 to 200 ms (default 40, first guess). Anything else ends the run with status 2. The top of the range is not usable in practice: at 200 ms the target equals the overflow mark (6546 frames), so any `Queue` that pushes the fill past the target overflows and drops, constantly. The range stays the spec's |
| `--audio-file FILE` | with `wav`: the file |

## The numbers, and which are guesses

| Number | Value | Status |
|---|---|---|
| the DSP's rate | 160 samples / 1,310,720 ticks | fixed (D63, ROADMAP) |
| the ring | 8192 frames | the spec's |
| the target fill | 40 ms | first guess |
| `--audio-latency` range | 10 to 200 ms | first guess |
| the overflow mark | 200 ms (6546 frames) | first guess |
| the underrun fade | 64 samples | first guess |
| the controller's bound | 0.995 to 1.005 | first guess |
| the tweak, the low-pass | 50 ms, 0.712 s | Azahar's constants |
| `Update`'s normalisation | `2 * target` | ours: Azahar's target is half its backlog |

Two behaviours follow from the guesses and are worth watching in the maintainer's runs. After an
overflow the fill is 200 ms, not the target, and the controller can bring it back only at 0.5 % of the
stream per second, about 33 s for 160 ms; the test allows 90 s. A device that opens long after the
game has been making sound starts at 200 ms for the same reason.

## Tests

`native/tests/audio_out_test.cpp`. The simulated-clock tests run a producer (one DSP frame per 1,310,720
ticks) and a device (480-frame blocks at 48,000 Hz) on a `ManualTimeSource`; every frame carries its sequence
number, so a lost, repeated or reordered frame shows. They cover: 10 simulated minutes with the device's clock
0.01 % fast and slow (the fill within 25 % of the target at every instant after 5 s, after each `Queue` and
before and after each `Pull`; no underrun, overflow or drop; the factor inside its bounds), the same at 0.3 %,
a 2 s stop of the producer (exactly one underrun, the fade, zeros, recovery), an overflow burst (dropped
and counted exactly, the fill back to the target), 10 simulated minutes with no consumer, the controller on
its own, two real threads for 10^6 frames with overflows and full-ring discards forced (nothing lost or
repeated but counted drops), the WAV file read back, and the factories, the options and `Find` through the
runtime.

The two-thread test is also run under ThreadSanitizer. The clang here has no TSAN runtime, so the recipe
is g++ 13 `-std=c++20 -O1 -g -fsanitize=thread` over `native/tests/audio_out_test.cpp`,
`native/src/audio_out/{stream_sink,drift_controller,wav_writer}.cpp`, the few sources the test needs
(`native/src/kernel/clock.cpp`, `native/src/common/{assert,log}.cpp`, `native/third_party/fmt/src/format.cc`,
`native/tests/support/{test_main,doctest_impl}.cpp`), with `native/include`, `native/src`, doctest and fmt on the
include path, into a build folder of its own, linked with `-lpthread`. Without `recomp.h` on the include path
the runtime-dependent cases are skipped (the ctest run covers them). A clean run prints no
`ThreadSanitizer` report. A relaxed `write_` or `read_` store in `stream_sink.cpp` makes it report a data race.
