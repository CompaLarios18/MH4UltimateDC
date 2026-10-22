# Our HLE DSP (N3-02 on)

The native runtime's own DSP, written in pieces: **N3-02** the sources, **N3-03** the mixers, the effects
and the frame (the `Dsp` object), **N3-11** the switch that replaces N1-18's model behind `dsp::DSP`
(its section is still to come), **N3-04** the replay of recorded captures.

The reference is Azahar's HLE DSP (`src/audio_core/hle/`, `codec.cpp`, `interpolate.cpp`), the pinned
commit in `port/azahar/COMMIT`. The bar is ROADMAP §6: decoding and interpolation bit-identical to it.
`docs/native/snd-tests.md` (N3-01), section *Things Azahar does*, lists what our programs show of it;
everything on that list is reproduced here.

## The module `native/src/dsp_hle/` (`mh4n_dsp_hle`)

Headers are included as `dsp_hle/<name>.h`, namespace `mh4n::dsp_hle`. It depends on `mh4n_audio` for the
layout of the shared memory (`audio/dsp_layout.h`), the status rules' constants and
`audio::IsPhysicalAddressValid`. Every file starts with the provenance header of AGENTS §8, rule 6
(GPL-2.0-or-later: each Azahar source says "Licensed under GPLv2 or any later version").

| File | Azahar source | What |
|---|---|---|
| `types.h` | `audio_types.h`, `hle/common.h` | `StereoSample`, `StereoFrame16` (160 stereo `s16`), `QuadFrame32` (160 × 4 `s32`) |
| `codec.{h,cpp}` | `codec.{h,cpp}` | `DecodePcm8`, `DecodePcm16`, `DecodeAdpcm` (GC-ADPCM: 8-byte frames of 14 samples, 11-bit coefficients, the history in `AdpcmState`) |
| `interpolate.{h,cpp}` | `interpolate.{h,cpp}` | `None` and `Linear` over a `StereoBuffer` (a consumed-from-the-front queue), `State` (two history samples and the 24-bit fixed-point position), `StepSize` |
| `filter.{h,cpp}` | `hle/filter.{h,cpp}`, `hle/common.h` | `SourceFilters`: the simple (15 fractional bits) and the biquad (14) filter, per source |
| `source.{h,cpp}` | `hle/source.{h,cpp}`, `hle/shared_memory.h` | `Source`: `Tick`, `MixInto`, `Reset`, `Sleep`, `Wakeup`; `SampleMemory`; `AdpcmCoefficients` |
| `effects.{h,cpp}` | `hle/effects.{h,cpp}`, `hle/shared_memory.h` | `DelayEffect` (delay with feedback, in Q7 coefficients), `ReverbEffect` (Azahar's does nothing), the blocks of the configuration |
| `mixers.{h,cpp}` | `hle/mixers.{h,cpp}`, `hle/shared_memory.h` | `Mixers`: the DSP configuration, the aux buses, the effects, the final mix; `DspConfiguration`, `IntermediateMixSamples` |
| `dsp.{h,cpp}` | `hle/hle.cpp`, `hle/aac_decoder.cpp`, `hle/decoder.h`, `hle/shared_memory.h` | `Dsp`: the model's interface, the pipes, the states and the frame (`GenerateCurrentFrame`) |

### `Source`

```
audio::SourceStatus Tick(audio::SourceConfiguration&, const AdpcmCoefficients&, SampleMemory&, u32 source_id);
void MixInto(QuadFrame32&, std::size_t mix) const;
void Reset(); void Sleep(); void Wakeup();
```

* `Tick` is Azahar's `Source::Tick`: it reads the configuration the application wrote (clearing the dirty
  bits in the structure it was given, as the DSP does in the shared memory), runs a frame if the source is
  enabled and returns the status. `AdpcmCoefficients` is the table of all 24 sources (768 bytes, the layout
  at `audio::shm::kAdpcmCoefficients`); the source takes row `source_id`.
* `MixInto` adds the source's last frame to an intermediate mix with the gains of mix 0, 1 or 2 (Azahar's
  conversion from stereo to quadraphonic, with the linear gain ramp over the first frame after a gain
  changed). Azahar's `MixInto` is not const (the ramp state advances); the ramp's state is `mutable` here
  so that N3-03's mixer can hold sources by const reference, as the WP declares it.
* `SampleMemory::Read(physical_address, size)` returns the bytes of guest physical memory. Whether an
  address is mapped is decided by `audio::IsPhysicalAddressValid` (Azahar's `GetPhysMemRegionInfo`: VRAM,
  DSP RAM, FCRAM of the New 3DS's size, the New 3DS's extra RAM, area ends included), as in N1-18's model,
  so the status sequences agree with it whatever the memory behind it. Inside a valid area, an empty or
  short span reads as zeros (an Old 3DS has no memory in the upper part of the FCRAM area, and Azahar's
  backing memory is zero there).
* The status semantics are those of `audio::SourceModel` (`native/src/audio/dsp_model.h` states them): the
  first frame after a source is enabled only dequeues and is silent; the queue is ordered by buffer id, the
  embedded buffer included; `buffer_position` is the sample number in the current buffer; the buffer's own
  `ps` is never read; ADPCM history is taken only when the buffer's dirty flag is set; a partial embedded
  update is ignored for ADPCM and PCM8 and honoured for PCM16 (a shrink below the play position resets the
  position to 0, one that sets `is_looping` does not loop a buffer that was not looping); rate 0 (or less)
  becomes 1.0; address low bits are masked; a looping PCM16 buffer's not-yet-consumed part is read again
  from memory every frame; polyphase plays as linear (Azahar's `TODO(merry)`), so `none` and `linear`
  coincide only at rate 1.0.

### Departures from Azahar

All deliberate, all because the DSP memory belongs to a game that must keep running. The first four are
N1-18's (`dsp_model.h` gives the details); the interpolation mode 3 in the first one, and 5 to 7, are new here.

1. Where Azahar asserts (a NaN rate multiplier, which passes its `<= 0` test; an interpolation mode of 3,
   for which it logs and then loops for ever), the source logs the error and goes on (a NaN becomes 1.0, mode
   3 plays as linear). Azahar's `UNIMPLEMENTED` only logs, and so does the source.
2. A frame dequeues at most `audio::kMaxDequeuesPerFrame` (1024) buffers; the rest of it stays silent and an
   error is logged (Azahar loops for ever on a looping buffer of length 0).
3. A play position past the end of a buffer empties it (Azahar erases past the end).
4. The address-validity rule above.
5. A buffer whose bytes in memory (`BufferByteExtent`: PCM8 1 or 2 per sample, PCM16 2 or 4, ADPCM 8 per 14
   samples) are more than `kMaxBufferBytes` (0x10000000, the New 3DS's whole FCRAM) is dropped with an error,
   as a buffer at an invalid address is. No memory area holds it, so Azahar can only read it out of bounds
   (it allocates up to 16 GB for a length of 2^31 - 1). Every buffer that fits in an area is played, a
   PCM16 stereo buffer of 2^25 + 1 samples or 40 million PCM8 samples included. A partial update to such
   a length is ignored in the same way. (N1-18's model has no such limit: the status sequences differ
   only for these lengths.)
6. The sums that overflow 32 bits in Azahar's C++ (the filters', and ADPCM's with extreme coefficients: signed
   overflow is undefined there) wrap, as the compiled code does; the harness is built with `-fwrapv`.
7. `Source::MixInto` of a mix other than 0 to 2 adds nothing (Azahar's `.at()` throws).

N3-03 adds, for the mixers and the effects (`mixers.h`, `effects.h` say the same):

8. An output format above 2 (a format value nobody defined: Azahar's `UNREACHABLE_MSG` ends the process when it
   mixes) is logged once per write and mixed as stereo, the format that Surround also falls back to. **Proposed
   addition to D78** (Azahar crashes).
9. The `float` to `s32` conversions of the downmix and of the delay (`static_cast<s32>` of a value that is out of
   range, or a NaN: a huge or NaN volume or gain, aux samples at the `s32` limits, an unstable delay whose filter
   overflows to infinity and then NaN) are undefined in Azahar's C++ and differ between hosts (x86 gives
   `INT32_MIN`, ARM saturates). `FloatToS32` (`types.h`) gives `INT32_MIN` for them on every host: what Azahar does
   on x86, its reference platform. In range it is the plain truncation. The oracle's table has no case in that
   range (the cases keep sums below 2^29); `dsp_hle_test.cpp` tests the conversion and the cases above. **Proposed
   addition to D78** (the result depends on the host). `Source::MixInto` (N3-02) still has the plain conversion for
   a gain of 2^31 or more in magnitude over a sample: not changed here.
10. The 28 padding bytes of `DspStatus` are an uninitialised local in Azahar that is copied into the shared
   memory (indeterminate); `Dsp` writes the two words it has and leaves the rest of the memory as it is, as the
   model does. The oracle does not hash them.
11. A wakeup without a sleep restores zeros, as the model (Azahar's backup is uninitialised memory).

The priority queue is a `std::priority_queue` ordered by buffer id, as Azahar's. Two buffers with one id play in
the order the standard library's heap gives (as in Azahar); no committed case has two.

## The oracle

`tools/native/dsp_oracle/dsp_oracle.py --azahar ~/azahar --cases native/tests/dsp_hle_cases.txt --out
native/tests/dsp_hle_expected.inc` (about 8 seconds since N3-03; the table has 153 cases):

1. copies Azahar's `codec.cpp`, `interpolate.cpp`, `filter.cpp`, `source.cpp` and (N3-03) `mixers.cpp` and
   `effects.cpp` with the headers they need (`audio_types.h`, `codec.h`, `interpolate.h`, `hle/common.h`,
   `hle/filter.h`, `hle/shared_memory.h`, `hle/source.h`, `hle/mixers.h`, `hle/effects.h`, and
   `common/bit_field.h`, all as they are) into a temporary folder; nothing of it is ever
   committed;
2. adds our stand-ins for the headers they include (`tools/native/dsp_oracle/standin/`, each saying what it
   replaces): `common/common_types.h`, `common/swap.h` (every little-endian type is the plain type),
   `common/assert.h` (a failed `ASSERT` aborts, `UNIMPLEMENTED` does nothing, as Azahar's only logs),
   `common/string_util.h` (empty: its only use is an argument of a discarded `LOG_DEBUG`),
   `common/common_funcs.h` (the padding macros), `common/logging/log.h` (nothing printed),
   `boost/serialization/{access,array,deque,priority_queue,vector}.hpp` (empty: the audio headers only name
   them for save states), and `core/memory.h` (a `MemorySystem` over the case's memory with Azahar's rule for
   which physical addresses are valid);
3. compiles them with `driver.cpp` and `driver_frame.cpp` using clang (`-O1 -ffp-contract=off -fwrapv`);
4. plays every source case of `dsp_hle_cases.txt` against one Azahar `Source`, the way `DspHle` calls it (`Tick` with
   the configuration and the source's row of coefficients, then `MixInto` for each of the three intermediate
   mixes), and writes, per case and frame, the CRC-32 of each of the three quad mixes and of the 12-byte
   status block; the frame cases (below) go through `driver_frame.cpp`.

The table is committed; no expected value is written by hand. A case is a seeded memory image and a list of
commands per frame (the cases file's header describes the language: `enable`, `rate`, `interp`, `format`,
`embedded`, `queue`, `partial`, `gain`, `filters`, `simple`, `biquad`, `coeffs`, `reset`, `partial_reset`,
`sleep`, `wakeup`, `poke`, and `rand:<seed>[:<max>]` for random coefficients). The script turns the commands
into byte writes into the 192-byte configuration (with the middle-endian 32-bit values and the dirty bits);
the test and the driver apply the same writes, so the table holds the inputs it was made from. The 121 source
cases are 200 frames each: each of PCM8 (mono, stereo), PCM16 (mono, stereo) and ADPCM against `none`, `linear` and
`polyphase` at rate multipliers 0.25 to 4.0; loops of every format; queues with ids out of order, zero and
tiny lengths; embedded play positions and partial updates (grow, shrink, below the position, ignored for
PCM8 and ADPCM); the simple and biquad filters with explicit and random coefficients, and enabling and
disabling them; ADPCM coefficient changes, history flags and a source other than 0; gain ramps; reset,
partial reset, sleep and wakeup; rate 0, negative, tiny and huge; unmapped, masked and area-end addresses; a
looping PCM16 buffer rewritten in memory.

`native/tests/dsp_hle_test.cpp` plays the same source cases on `mh4n::dsp_hle::Source` and compares all four CRCs of
every frame; it first checks that its copy of the data generators makes the bytes whose CRC-32 the table
holds. `tests/test_native_dsp_oracle.py` tests the case reader, the commands and the table writer on synthetic
cases, and (with `MH4N_AZAHAR_DIR` at the pinned commit and clang++) runs the harness twice and compares with
the committed table byte for byte. Adding a case: edit `dsp_hle_cases.txt`, run the script, commit both.

### The second oracle

For 50 seeded random configurations over 2000 frames (the generator mixes resets, partial resets, enables,
sync counts, rates including 0 and negative, interpolation modes, formats including 3, embedded buffers with
play positions past the end, queued buffers with valid and invalid addresses and lengths from 0 to 1000,
partial updates, sleep and wakeup), `Source` and `audio::SourceModel` give identical status sequences and
leave the same dirty words behind.

## The mixers, the effects and the frame (N3-03)

`mh4n::dsp_hle::Dsp` (`dsp.h`) has the public interface of `audio::DspModel` (N1-18): the constructor on the
0x80000 bytes of DSP memory, `SetInterruptHandler`, `SetFrameStartHook`, `State`, `RecvData`,
`RecvDataIsReady`, `SetSemaphore`, `PipeRead`, `PipeReadableSize`, `PipeWrite`, `Tick` and
`CurrentRegionIndex`, plus `SetSampleMemory(SampleMemory*)` (where the sources' samples are; without one every
buffer reads as zeros) and `SetFrameOutput(std::function<void(std::span<const s16>)>)` (the 160 stereo
samples, 320 interleaved `s16`, of each frame generated, after they are written to the shared memory).

`Tick` is Azahar's `GenerateCurrentFrame`: the region with the higher frame counter is read, the other written.

1. each of the 24 sources is ticked on its configuration in the read region (the dirty bits cleared there) and
   the table of ADPCM coefficients, writes its status to the write region, and is mixed into the three
   intermediate mixes (`MixInto`, in that order, source by source);
2. `Mixers::Tick` reads the DSP configuration of the read region (the dirty bits cleared, the delay effects'
   dirty words too), takes the aux buses' samples that the application left in the **read** region
   (`AuxReturn`, a bus that is enabled), writes the sources' mixes for the aux buses into the **write** region
   (`AuxSend`; a bus that is not enabled is mixed directly), so the echo of the application's processing is
   one frame late; applies the delay and the reverb (which does nothing) to the two aux mixes; and mixes the
   three with the master volume and the two aux return volumes (all start at 0) as mono, or stereo (surround is
   stereo too), saturating each sample at 16 bits where Azahar does;
3. the DSP status (two zero words), the source statuses and the final samples go to the write region, and the
   frame goes to the output.

The pipes, the states, the structure addresses, the region rule and the whole-memory backup of Sleep and Wakeup
are the model's. The binary pipe answers as the model does and as Azahar's AAC decoder does when a decode
fails (48000 Hz, 2 channels, 1024 samples, result error); the first decode request logs `dsp: AAC decode
requested (not supported)` at Warning, and `AacDecodeRequests()` counts them all (N3-11, N3-26 and N3-27 read it
and stop with `needs-decision` if a game makes any).

The mixers add `-ffp-contract=off` to their compile flags (as `source.cpp`): the oracle is built without fused
multiply-add, and the expressions are Azahar's, with the order of the additions.

### The frame cases of the oracle

A case with `mode=frame` is played by `tools/native/dsp_oracle/driver_frame.cpp`, which holds **Azahar's own
`GenerateCurrentFrame`** (about 25 lines of `hle.cpp`, copied with its provenance, with `CurrentRegionIndex`,
`Initialize`, `Sleep` and `Wakeup`; `hle.cpp` itself needs the whole `DspHle`) around Azahar's `Source`,
`Mixers` and effects compiled from the real files (`mixers.cpp`, `effects.cpp` with stand-ins for
`common/string_util.h` and the others). The driver plays the application: before frame *k* it writes the case's
events into the region the DSP reads next (region (*k* + 1) mod 2), sets that region's frame counter to
`fc + k + 1` (a case can start the counters close to 0xFFFF: `fc=65500`) and runs the frame. Per frame it prints
four CRC-32: the final samples of the write region; the 24 source statuses and the two words of the DSP status;
the write region's aux buses' samples; and the read region's source configurations and DSP configuration (the
dirty bits the frame cleared). A frame in which the DSP is asleep prints zeros. The commands are in the script's
docstring (`s<k> <source command>`, `master`, `auxreturn`, `outfmt`, `auxbus`, `delay`, `reverb`, `auxmix`,
`headphones`, `dspdirty`, `dspwrite`, `sleep`, `wakeup`, `poke`); the aux buses' samples come from a seeded
generator, the table holds its parameters and the CRC-32 of its output.

The table keeps the source cases' rows byte for byte; a frame case's row has the first frame counter in the
third number and ends with `, 1`. `dsp_hle_test.cpp` plays each frame case on `Dsp`, through the audio pipe and
the shared memory as the driver does, and compares all four CRCs of every frame. The frame cases: the master
volume (never set, changed, above 1, negative), the routing of a source to each of the three mixes with the aux
return volumes, gain ramps, the three output formats, saturation (24 sources of full-scale noise, a master volume
of 40), the aux buses (enabled, disabled, toggled, the application's samples read back a frame late, a bus whose
enable has no dirty bit), the delay on each aux mix with 1, 2 and 5 frames and changing coefficients, both
delays with both buses enabled, the delay's edges (frame count 0, the 4-second clamp at 819 frames, disabling,
a new configuration of the same length, an effect whose own dirty word or whose main dirty bit is not set), the
reverb, the unimplemented dirty bits, the wrap-around of the frame counters, sleep and wakeup, source statuses
and stops, and **24 sources at once** (every format, rate and interpolation mode, filters, three mixes, both aux
buses with delays, mono and stereo). Cases are 200 frames; the delay is kept stable (|b| < 1) and sums below
2^29 so that no case goes through a float-to-`s32` conversion that is undefined in Azahar (departure 9).

Numbers: **153 cases** (121 source cases, 32 frame cases) of 200 frames each, **8 seconds** for the
harness.

### The second oracle: the memory against N1-18's model

For 12 seeded random sequences of 400 frames, `Dsp` and `audio::DspModel` run side by side on separate memories
that receive the same bytes from the "application": state changes on the audio pipe (initialise, shutdown,
wakeup, sleep, unknown), binary pipe messages of every codec, command and size, pipes that are not implemented,
reads of random lengths; source configurations of 24 sources (the generator of the status test), tables of ADPCM
coefficients, DSP configurations (volumes including NaN, infinity and 1e30, output formats including ones that
are not formats, aux buses, delay effects with random coefficients and frame counts up to 65535, reverb blocks,
random dirty bits), aux bus samples, and frame counters at random including the wrap-around edges. After every
tick both have the same state, region index, interrupts, frame-start calls, pipe contents and **shared memory,
byte for byte, outside the final samples and the intermediate mix samples of both regions**, which the model
leaves silent and `Dsp` writes.

## For N3-11

* `Dsp` replaces `audio::DspModel` where the service holds it: the same calls, plus `SetSampleMemory` (a
  `SampleMemory` over the runtime's guest memory only has to return the bytes (`FCRAM`, `VRAM`, `DSP RAM`);
  validity is decided by the source) and `SetFrameOutput` (the sink of N3-05, `audio_out::Stream::Queue`).
* The frame is generated by `Tick()`, which returns whether the DSP was on; the caller raises the audio pipe's
  interrupt as it does with the model.
* `AacDecodeRequests()` is what N3-11, N3-26 and N3-27 count.
