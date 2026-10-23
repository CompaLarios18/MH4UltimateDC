# The audio output stream (N3-05)

What sits between the native DSP and an audio device, with no SDL: `native/src/audio_out/`
(`mh4n_audio_out`). The DSP runs on the scheduler thread and makes sound in step with the game
clock; the device runs on a thread of its own and wants samples at its own pace. The stream
between them never makes the game wait (D63). The SDL3 device is N3-06 (`native/app/audio/`,
`mh4n_app_audio`, in `mh4u-native` only): its section is *The SDL3 device* below.

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

## The SDL3 device: `--audio sdl` (N3-06)

`native/app/audio/` (`mh4n_app_audio`, an app module: only `mh4u-native` and the tests under `native/app/tests/` link it;
nothing under `native/src/` sees SDL). It registers the `AudioSinkFactory` `sdl`, which makes the `stream` sink through
its registered factory (so `--audio-latency` works as before and a bad value ends the run with status 2), finds the stream
with `audio_out::Find`, and plays it on the player's default audio device.

**Opening.** In the `devices` boot step, before the game has made any sound (a device that opens late finds old audio, see
*Resuming*): `SDL_InitSubSystem(SDL_INIT_AUDIO)` and `SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, {S16, 2,
32728}, Feeder::Callback)`, then the device is resumed (SDL opens it paused). The spec is the app's side of the stream; SDL
chooses the device's format (never assume it: the audio log's header says its rate) and converts and resamples. The device
and the subsystem are closed when the run ends (`Runtime::OnFinish`). A device SDL cannot open, or an audio subsystem that
cannot start (no driver), logs a **Warning** (`AudioSdl: ... the game plays without sound`) and the sink plays nothing: the
stream's ring stays full, counts what it discards in `dropped`, and `Queue` never blocks; the run goes on, `Status` has no
device and `SetVolume` returns false.

**The callback** (`app_audio::Feeder::OnRequest`, SDL's get callback, on SDL's audio thread with the SDL stream's lock held;
it calls only `StreamSink::Pull`, the drift controller and SDL's stream functions: no allocation, no logging, no wait;
counters only). SDL tells it how many bytes of the stream's input format (S16 stereo at 32,728 Hz) it needs now, and the
callback:
1. does nothing when SDL needs less than a frame (it has enough queued);
2. after an overflow of the stream, throws the excess away (*Resuming*);
3. steers as *How a consumer steers* says: the fill minus half the block, the time since the previous request (steady clock),
   `Reset()` and the factor 1 while `Priming()`; then `SDL_SetAudioStreamFrequencyRatio(stream, (32,728.498 / 32,728) *
   factor)`;
4. pulls the block and `kMarginFrames` (2) more, in chunks of at most 4096 stream frames (one `Pull` of more than 6546 can never
   be satisfied), and puts them into the SDL stream. `Pull` makes up for what the ring lacks (the fade, then zeros), so SDL
   always gets all it asked for.

Three things about SDL 3.4.18 that the numbers depend on (read in `SDL_audiocvt.c`, checked by the tests):
- **The ratio moves the source rate in whole hertz.** SDL computes `(int)(32728 * ratio)` as the stream's source rate, so the
  effective rate is 32,728, 32,729, ... (30 ppm a step). The base ratio 32,728.498 / 32,728 therefore plays at 32,728 Hz
  while the DSP makes 32,728.498: the controller has to keep its factor at 1.0000153 or more to reach 32,729, and the closed
  loop dithers between two neighbouring rates. The tests (a device exactly on the DSP's clock, 0.01 % and 0.3 % off) keep the
  fill inside 25 % of the target with it.
- **The margin.** SDL works out its request with the ratio of the *previous* block and the callback changes the ratio before
  the block is taken; about one block in a hundred then needs one frame more than was asked, and SDL gave its device a block
  with a frame missing (silence for that frame, a tick). The margin of 2 frames removes it (what SDL does not use stays in its
  queue and is counted in its next request, so it does not pile up): 0 short blocks in 10 simulated minutes, with the margin;
  607 without it, at the same settings.
- **The first block** after the SDL stream is created is up to about 10 output frames short (the resampler has no history
  yet). The stream is priming then, so it is silence either way.

**Resuming.** The stream's ring keeps the newest 200 ms when nobody pulls (above). A device that opens late, or that resumes
after the system moved the stream to another device or paused it, would start at the mark and the controller would need
about 33 s to bring the fill back to the target. The callback therefore remembers the stream's `overflows` count: when it
has changed since the previous request (or since the feeder was made), it throws away what the ring holds above `max(target +
block / 2, block)` frames (the block is in the middle of the target again) and, if it threw anything away, resets the controller. The frames thrown away
are `DeviceStatus::discarded_frames`. A pause shorter than the time the ring needs to reach its mark (about 160 ms at the
default target) leaves up to that much extra in the ring and is left to the controller. The device is opened early so that
the first case does not arise.

**Device changes.** A stream opened on `SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK` follows the system's default device by itself: when
the player switches the default output, or the device in use is unplugged and another default exists, SDL moves the stream and
converts to the new format; when no device is left it keeps consuming into nothing until one appears. The app does nothing
for that. SDL tells it with events, and an SDL event watch logs them: `SDL_EVENT_AUDIO_DEVICE_REMOVED` of a playback device is
a **Warning** (`an audio output device was removed`), `ADDED` and `FORMAT_CHANGED` are Info lines. SDL posts these when the
app pumps its events (`SDL_PumpEvents`, `SDL_PollEvent`: the app's main loop does, N3-14); a run that never pumps them
(the headless runner) logs nothing and the stream still follows the default device.

**Volume.** `--volume N` (0 to 100, default 100; anything else ends the run with status 2) and `app_audio::SetVolume(Runtime&,
int percent)` (any thread, any time; `percent` is clamped; returns false when the runtime has no open SDL device, in which
case nothing is remembered: `--volume` is what a device starts with) set the gain `(N / 100)^3` through
`SDL_SetAudioStreamGain`, Azahar's curve (`dsp_interface.cpp`: the cube of the linear volume, approximating a linear change in
perceived loudness). 100 is 1, 50 is 0.125 (-18.06 dB), 25 is 0.0156 (-36.1 dB), 0 is silence (the callback goes on pulling,
so the ring stays in step). SDL applies a new gain to the next block without a ramp. `app_audio::Status(Runtime&)` gives the
device's rate, the volume and gain, the ratio and factor, the fill the controller last steered on, the callbacks served and
the frames discarded (for the UI and the tests).

**`--audio-log FILE`** writes a CSV from a helper thread (never from the callback), one row per second of host time:

```
# audiolog v1 2026-10-23 device_rate=44100
t_ms,frame,fill_frames,ratio,underruns,overflows,dropped
0,0,0,1.000015,0,0,0
1000,1204,1326,1.000321,0,0,0
...
```

`t_ms` is the host's steady clock since the device was opened (the first row is at 0, the next ones at 1000, 2000, ... ms, and
a last row is written when the run ends, so the last row has the run's final counters); `frame` is `audio_out::LastFrame()` (the
game's frame counter: `perf_report.py` cuts the log where a scene starts); `fill_frames` is the ring's `Fill()` at that
instant (it swings by one block around the target: 40 ms plus or minus half a 10 ms block, 982 to 1636 frames); `ratio` is the
frequency ratio the callback last set; `underruns`, `overflows` and `dropped` are `Stats()`'s. The header's `device_rate` is the
device's rate when the log was opened (0 when the device did not open: the log is still written, so a run without sound shows
`dropped` growing). A log that cannot be created ends the run with status 2, like the other trace files.

### Options added by N3-06 (runner scope, `mh4u-native` only)

| Option | Meaning |
|---|---|
| `--audio sdl` | the stream played on the default audio device (`--audio` itself is the runtime's) |
| `--volume N` | with `sdl`: the volume, 0 to 100 (default 100), gain (N / 100)^3 |
| `--audio-log FILE` | with `sdl`: the audio log above |

(`--audio-latency` is the stream's, and sets the ring's target.) Latency a player hears is the ring's target (40 ms), plus SDL's
own queue (a few frames), plus the device's buffer: SDL's default is 1024 frames for a device up to 48 kHz (about 21 to 23 ms);
`SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES` would lower it, and N3-06 does not set it.

### Tests of the device

`native/app/tests/app_audio_test.cpp`, with SDL's drivers forced by override hints (so a `SDL_AUDIO_DRIVER` in the environment
changes nothing):
- **the feeder on a manual clock**, around an SDL audio stream that no device owns (SDL's own converter, resampler and
  callback, with `SDL_GetAudioStreamData` as the device): 10 simulated minutes with the device's clock 0.01 % fast, 0.01 % slow,
  exactly on the DSP's, and 0.3 % fast and slow keep the fill inside 25 % of the target after 5 s with no underrun, overflow,
  drop or short block, the ratio inside its bounds (and, at 0.3 %, away from 1 the right way); a 2 s stop of the producer gives
  one underrun, silence, and recovery; a device that is away for 1 s and comes back plays from the target within 20 blocks
  (discarding 5000 frames) instead of 33 s; requests below a frame, and above the mark, are served right;
- **`disk` driver** (`SDL_AUDIO_DISK_OUTPUT_FILE`): a 1 kHz tone queued at 32,728.498 Hz for 2 s of real time comes out at the
  device's rate and format (read with `SDL_GetAudioDeviceFormat`) at 1 kHz (zero crossings: the median interval, within 1 %) and
  its RMS within 1 dB of the input; `--volume 50` is 18.1 dB lower (+-0.5 dB), 25 is 36.1 dB lower, 0 is silence;
- **`dummy` driver**: the controller keeps the fill it steers on inside 25 % of the target from 5 s to 12 s, and the audio log's
  header, columns, rows (one per second, the frame column, a last row) are checked. The dummy and disk drivers sleep a whole
  number of milliseconds per block (1764 frames at 44,100 Hz is 40 ms exactly; the default 1024 is 23.2 ms, which they sleep as
  23, 1 % fast) plus the host's wake-up latency, so their clocks are off by more than the controller's +-0.5 % on some hosts: the test
  paces the DSP by the device's own clock (the callbacks it served) and makes it 0.3 % fast, which is the drift the controller has
  to follow, and reports how far the host's clock was from the device's (0.03 to 0.4 % slow on the cloud machine, depending on how busy
  it was); the disk test's producer is paced the same way, and its tone comes out about 0.35 % high (1003.5 Hz): the ring is 250 frames above the target
  when the stream starts, and the controller works at its +0.5 % limit for the first 2 s to bring it down, which is all of the 2 s the
  test plays (the Done-when's 1 % holds with room);
- devices that cannot open (an audio driver that does not exist, a disk file that cannot be created: a Warning, no sound, the ring
  full and counting, nothing blocks), the options (`--volume` and `--audio-latency` out of range, an `--audio-log` that cannot be
  written: status 2), `SetVolume` and `Status`, the device closed and forgotten at the end of the run, SDL's device events;
- `MH4N_AUDIO_LISTEN=1` turns on one more case, the listening test below; it is skipped otherwise.

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

## Listening test on a real device (pc)

A person at a real output device is the only judge of clicks, drift and what a device switch sounds like, and nothing else in
N3-06 can say. This is the procedure for the maintainer (`pc`, N3.md rule 6, N2b.md rule 10): numbered steps, each with what to
expect and about how long it takes; what he sends back; what to do when a step fails.

**What can be run today and what cannot.** `mh4u-native` answers only `--help` and `--version` until N2b-10 and N3-14
(`docs/native/app.md`), so the packaged exe cannot play anything yet. What can be played today is `app_audio_test`, the test
program of this package, which has an opt-in listening case: with `MH4N_AUDIO_LISTEN=1` it plays the 1 kHz tone through the real path
(the runtime's `--audio sdl` sink, the stream, the drift controller, SDL's resampler, the player's default Windows output device),
at volume 100, 50, 25 and 0, then keeps playing while he switches and unplugs devices, and writes the audio log. Without the variable
the case is skipped, so `ctest` never plays a sound. Its steps are 1 to 10 below. The steps that need the game's own sound and the
app's controls need N3-14's wiring and are written as "N3-14 fills in the command" (steps 11 to 13): they are not runnable now.

**`app_audio_test.exe` is not in the artifact yet.** The Windows job of `native-app.yml` builds and runs `app_audio_test` but publishes only
the six files of `docs/native/app.md`. Until the orchestrator adds `app_audio_test.exe` to the folder the job zips (a one-line change in
the step "Package mh4u-native and its DLLs", proposed in the report of N3-06), the alternative is to build it on the PC with the
toolchain of N2b-11 (`cmake --build build\native-app --target app_audio_test`, the configure line of `docs/native/app.md`). If
neither exists, **the listening test waits for N3-14**, which will have `mh4u-native.exe` play for itself.

Needed: Windows 10 or 11, two audio output devices that can both be the default (the speakers or headphones on the PC, and a second
one: HDMI, USB, Bluetooth or the other jack), and about 40 minutes. Start with the system volume low (about 20 %): the tone is at half
of full scale.

1. **Get the program** (3 min). From the Windows artifact of the run the orchestrator names (`docs/native/app.md`, *Getting
   `mh4u-native.exe`*), unzip it into `C:\NoJales\mh4pcport\audio-listen`:
   ```
   Set-Location C:\NoJales\mh4pcport\repo
   New-Item -ItemType Directory -Force C:\NoJales\mh4pcport\audio-listen, C:\NoJales\mh4pcport\perf\n3\audio-listen | Out-Null
   Expand-Archive -Force -Path "$HOME\Downloads\mh4u-native-windows.zip" -DestinationPath C:\NoJales\mh4pcport\audio-listen
   Get-ChildItem C:\NoJales\mh4pcport\audio-listen | Select-Object Name, Length
   ```
   *Expect:* `app_audio_test.exe` and `SDL3.dll` in the list, with the files of `app.md`'s table. *Fails if:* `app_audio_test.exe` is
   missing: stop and tell the orchestrator (the artifact was made before the change above): the test waits.
2. **The program starts and skips** (1 min).
   ```
   Set-Location C:\NoJales\mh4pcport\repo
   Set-Location C:\NoJales\mh4pcport\audio-listen
   Remove-Item Env:MH4N_AUDIO_LISTEN -ErrorAction SilentlyContinue
   .\app_audio_test.exe -tc="listen*"; "exit $LASTEXITCODE"
   ```
   *Expect:* `test cases: 1 | 0 passed | 0 failed | 1 skipped` (or `0 passed ... 1 skipped`), `Status: SUCCESS!`, `exit 0`, and **no sound**.
   *Fails if:* a dialog about a missing DLL (send its name), or a sound plays.
3. **Set up the output** (2 min). In Windows' sound settings choose output device A (speakers or headphones) as the default output, set its
   volume to about 20 %, close every program that makes sound, and keep device B ready. Wait until the Windows volume meter is still.
   *Expect:* silence. (Nothing to send.)
4. **The tone and the volume steps** (about 1 min 40 s).
   ```
   Set-Location C:\NoJales\mh4pcport\repo
   Set-Location C:\NoJales\mh4pcport\audio-listen
   $env:MH4N_AUDIO_LISTEN = '1'; $env:MH4N_AUDIO_LISTEN_STEP_S = '10'; $env:MH4N_AUDIO_LISTEN_MINUTES = '1'
   $env:MH4N_AUDIO_LISTEN_LOG = 'C:\NoJales\mh4pcport\perf\n3\audio-listen\short.csv'
   .\app_audio_test.exe -tc="listen*" 2>&1 | Tee-Object -FilePath C:\NoJales\mh4pcport\perf\n3\audio-listen\short.txt
   ```
   *Expect* (the console prints `listen: t=<s> s: volume <N>` at each change, and `listen: t=<s> s fill=... underruns=...` every 5 s):
   - 0 to 10 s, volume 100: a steady, pure 1 kHz tone on device A. Listen for clicks, pops, crackle, a warble or a wavering pitch: there
     should be none, from the first moment to the last.
   - 10 to 20 s, volume 50: clearly quieter (18 dB: about an eighth of the amplitude); 20 to 30 s, volume 25: much quieter, faint (36 dB);
     30 to 40 s, volume 0: silence, no tone and no hiss; 40 to 50 s, volume 100 again: as loud as at the start. SDL changes the level at
     the next block without a ramp, so a faint tick at a change is normal; a loud pop is not.
   - 50 to 110 s: the tone at volume 100 (step 5 does its work here, so read step 5 before starting).
   - the last line is `listen: done. underruns=0 overflows=0 dropped=0` and the program ends, `[doctest] Status: SUCCESS!`.
   *Fails if:* no tone at all (send the console log), clicks or crackle in the steady parts (note the times), a level that does not follow
   the steps, an `underruns` above 0 after the first 10 s, or any `[Warning]` line.
5. **Switch the default device while it plays** (inside step 4's last minute, 50 to 110 s of the run; 2 min on its own). When the console
   says `volume 100 (free run: switch or unplug the output device now)`, in Windows' sound settings make device B the default output, wait
   10 s, then make A the default again, wait 10 s. *Expect:* the tone moves to B within about 2 s (a gap of up to 1 s while Windows and SDL
   switch is fine; a short noise burst or a tone that never comes back is not), plays on B without clicks, and comes back to A the same way.
   The console may show `[Info] AudioSdl:` lines about devices added or format changed (SDL also reports the devices it finds when it starts, so one or two
   `added` lines at the start are normal; no line is also fine). `underruns` may go up by one
   or two at a switch and must not go up between switches. *Fails if:* the tone stays silent after either switch for more than 5 s, a loud
   noise, a crash, or the program exits.
6. **Unplug the device in use** (3 min). Repeat step 4 with `$env:MH4N_AUDIO_LISTEN_MINUTES = '2'` and the log `unplug.csv` (a name of its own,
   and `unplug.txt` for the console). In the free run make device B (USB, Bluetooth or HDMI: one you can unplug or switch off) the default and wait
   until the tone plays on it, then unplug or switch it off. *Expect:* a line `[Warning] AudioSdl: an audio output device was removed`
   in the console (the program must be running its event loop for SDL to say so, and it is); the tone moves to the next default device or
   stops, without a crash and without the program exiting; the game-side counters go on (`dropped` may grow while nothing plays). Plug
   the device in and make it the default again: the tone returns on it within about 3 s. *Fails if:* the program exits or hangs, no Warning line
   appears at all, or the tone does not come back after the device is back and made the default (note whether it comes back after you
   make another device the default and back again).
7. **No drift over 10 minutes** (13 min). Do not touch the devices or the volume.
   ```
   Set-Location C:\NoJales\mh4pcport\repo
   Set-Location C:\NoJales\mh4pcport\audio-listen
   $env:MH4N_AUDIO_LISTEN = '1'; $env:MH4N_AUDIO_LISTEN_STEP_S = '1'; $env:MH4N_AUDIO_LISTEN_MINUTES = '10'
   $env:MH4N_AUDIO_LISTEN_LOG = 'C:\NoJales\mh4pcport\perf\n3\audio-listen\long.csv'
   .\app_audio_test.exe -tc="listen*" 2>&1 | Tee-Object -FilePath C:\NoJales\mh4pcport\perf\n3\audio-listen\long.txt
   ```
   (The first 5 s run the volume steps at 1 s each; the other 10 minutes are the tone at volume 100.) Listen from time to time: the tone stays steady and
   clean for the whole 10 minutes, with no drop-out, click or change of pitch. *Expect* at the end `listen: done. underruns=0 overflows=0 dropped=0`.
   *Fails if:* a drop-out or crackle (note the time), or `underruns`, `overflows` or `dropped` above 0 at the end.
8. **Read the log of step 7** (2 min).
   ```
   Set-Location C:\NoJales\mh4pcport\repo
   $r = Get-Content C:\NoJales\mh4pcport\perf\n3\audio-listen\long.csv | Select-Object -Skip 1 | ConvertFrom-Csv
   "rows: $($r.Count)"; $r | Select-Object -Last 1 | Format-List
   $r | Where-Object { [int]$_.t_ms -ge 10000 } | Measure-Object -Property fill_frames -Minimum -Maximum -Average | Format-List
   $r | Measure-Object -Property ratio -Minimum -Maximum | Format-List
   ($r | Where-Object { [int]$_.t_ms -ge 10000 } | Select-Object -First 60 | Measure-Object -Property fill_frames -Average).Average
   ($r | Select-Object -Last 60 | Measure-Object -Property fill_frames -Average).Average
   ```
   *Expect:* about 607 rows; the last row has `underruns`, `overflows` and `dropped` 0; `fill_frames` from 10 s on between about 800 and 1800
   (the target is 1309 frames, 40 ms; the fill swings by half a device block around it: 180 frames for a 10 ms block, 380 for SDL's 21 to 23 ms); `ratio` between 0.995 and 1.0051 (in practice within 0.2 %
   of 1.000015); the two averages (the first 60 rows after 10 s, the last 60) within 330 frames of each other (25 % of the target): that is "no
   drift". *Fails if:* any of these is outside: send the numbers.
9. **Quality at the other device** (3 min). Repeat step 4 once more with device B as the default from the start (no switching), log `deviceb.csv`.
   *Expect:* the same as step 4. The `device_rate` in the log's first line (`# audiolog v1 ... device_rate=<Hz>`) is the rate of B's format in
   Windows' sound settings (44100 or 48000 for most devices; send it).
10. **Notes** (2 min). Write one line each: the Windows version, the two devices (kind, not the serial numbers), the `device_rate` of A and of B, and
    for each of steps 4 to 9 "ok" or what you heard and when.
11. **The game's own sound** (N3-14 fills in the command). `mh4u-native.exe` with `--audio sdl` playing the game's title screen and a quest:
    music and effects clear, no clicks, in step with the picture. *Expect:* as steps 4 and 7 (no clicks, `underruns` 0 after the first 10 s of the
    audio log). *Needs N3-14* (the app plays the game and takes the options).
12. **The volume and mute controls** (N3-14 fills in the command). `--volume 50` at start, then the volume slider and the mute of the app's menu while the
    game plays: each change is heard at once, at the (N / 100)^3 curve, and a muted game is silent. *Needs N3-14* (`app_audio::SetVolume` wired to the UI).
13. **Switching the device with the game running** (N3-14 fills in the command). Step 5 and 6 with the game's sound instead of the tone. *Needs N3-14* (the
    app pumps SDL's events in its window loop; the game's `--audio-log` is `perf_report.py --audio-log`'s input, N3-27).

**What you send back** (in the batch PR or the chat; text only, never screenshots or captures): the lines of step 10, the files `short.txt`, `short.csv`,
`unplug.txt`, `unplug.csv`, `long.txt`, `long.csv`, `deviceb.csv` (timings and counters only: they hold no game data), and the output of step 8.

**When a step fails:** stop there. Send the step's number and the last 50 lines of the console log the step names (`short.txt`, `unplug.txt` or `long.txt`;
for step 2 the console text), and say what you heard and at about which second. Do not retry with other settings: the orchestrator reads the log first.
Whatever step 8's numbers say, none of the limits of this procedure is a pass or fail threshold of N3-06 (N3.md rule 7: the 40 ms target, the 0.5 % bound,
the 200 ms mark and the 64-sample fade are first guesses): a result outside them is reported as a `needs-decision` table, and nobody retunes the numbers.
