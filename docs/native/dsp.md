# Our HLE DSP (N3-02 on)

The native runtime's own DSP, written in pieces: **N3-02** (this page) the sources, **N3-03** the mixers,
the effects and the frame, **N3-11** the switch that replaces N1-18's model behind `dsp::DSP`, **N3-04**
the replay of recorded captures. N3-03 and N3-11 add their sections here.

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

The priority queue is a `std::priority_queue` ordered by buffer id, as Azahar's. Two buffers with one id play in
the order the standard library's heap gives (as in Azahar); no committed case has two.

## The oracle

`tools/native/dsp_oracle/dsp_oracle.py --azahar ~/azahar --cases native/tests/dsp_hle_cases.txt --out
native/tests/dsp_hle_expected.inc` (about 6 seconds):

1. copies Azahar's `codec.cpp`, `interpolate.cpp`, `filter.cpp` and `source.cpp` with the headers they need
   (`audio_types.h`, `codec.h`, `interpolate.h`, `hle/common.h`, `hle/filter.h`, `hle/shared_memory.h`,
   `hle/source.h`, and `common/bit_field.h`, all as they are) into a temporary folder; nothing of it is ever
   committed;
2. adds our stand-ins for the headers they include (`tools/native/dsp_oracle/standin/`, each saying what it
   replaces): `common/common_types.h`, `common/swap.h` (every little-endian type is the plain type),
   `common/assert.h` (a failed `ASSERT` aborts, `UNIMPLEMENTED` does nothing, as Azahar's only logs),
   `common/common_funcs.h` (the padding macros), `common/logging/log.h` (nothing printed),
   `boost/serialization/{access,array,deque,priority_queue,vector}.hpp` (empty: the audio headers only name
   them for save states), and `core/memory.h` (a `MemorySystem` over the case's memory with Azahar's rule for
   which physical addresses are valid);
3. compiles them with `driver.cpp` using clang (`-O1 -ffp-contract=off -fwrapv`);
4. plays every case of `dsp_hle_cases.txt` against one Azahar `Source`, the way `DspHle` calls it (`Tick` with
   the configuration and the source's row of coefficients, then `MixInto` for each of the three intermediate
   mixes), and writes, per case and frame, the CRC-32 of each of the three quad mixes and of the 12-byte
   status block.

The table is committed; no expected value is written by hand. A case is a seeded memory image and a list of
commands per frame (the cases file's header describes the language: `enable`, `rate`, `interp`, `format`,
`embedded`, `queue`, `partial`, `gain`, `filters`, `simple`, `biquad`, `coeffs`, `reset`, `partial_reset`,
`sleep`, `wakeup`, `poke`, and `rand:<seed>[:<max>]` for random coefficients). The script turns the commands
into byte writes into the 192-byte configuration (with the middle-endian 32-bit values and the dirty bits);
the test and the driver apply the same writes, so the table holds the inputs it was made from. 116 cases of
200 frames: each of PCM8 (mono, stereo), PCM16 (mono, stereo) and ADPCM against `none`, `linear` and
`polyphase` at rate multipliers 0.25 to 4.0; loops of every format; queues with ids out of order, zero and
tiny lengths; embedded play positions and partial updates (grow, shrink, below the position, ignored for
PCM8 and ADPCM); the simple and biquad filters with explicit and random coefficients, and enabling and
disabling them; ADPCM coefficient changes, history flags and a source other than 0; gain ramps; reset,
partial reset, sleep and wakeup; rate 0, negative, tiny and huge; unmapped, masked and area-end addresses; a
looping PCM16 buffer rewritten in memory.

`native/tests/dsp_hle_test.cpp` plays the same cases on `mh4n::dsp_hle::Source` and compares all four CRCs of
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

## For N3-03 and N3-11

* The frame (`GenerateCurrentFrame`): tick the 24 sources, zero three `QuadFrame32`, `MixInto` each source
  for mixes 0 to 2 (the sources keep their gain ramps), then the mixers.
* `AdpcmCoefficients` is read from the shared memory with a `memcpy` (the host is little-endian).
* A `SampleMemory` over the runtime's guest memory only has to return the bytes (`FCRAM`, `VRAM`, `DSP RAM`);
  validity is decided by the source.
