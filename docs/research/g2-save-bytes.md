# What the 24 bytes of G2's save are

N0-31 (decision D52). In G2 the game writes its save slot with one `fs:USER/file` Write of 81,408 bytes
at offset 0 (request header `0x08030102`, thread `#13` by cross-model naming, the file `user1` of the
title's save data). N0-28 found that the buffers of `jit` and `recomp` differ in 24 bytes, in two ranges,
offsets 0 to 7 and 280 to 295, while each backend repeats its own bytes exactly
(`docs/native/cross-model.md`, section 7). This note says what those bytes are.

Only offsets, sizes, frames, function addresses, clock values and algorithm names are written here. The
save, the dumps and the traces are game data and stay in `build/`; no byte of them, no instruction text
and no string of the game appears below (the cipher key is "the string at 0x00e05e04 of the code").
The tool is `tools/native/save_probe.py`.

## Result

| Ciphertext bytes | What they are | Why they differ between `jit` and `recomp` | Certainty |
|---|---|---|---|
| 0 to 7 (ECB block 0) | the slot's header: a format word and the **byte-sum checksum** of everything from plaintext offset 8 to the end | the sum changes because the bytes of the other range changed | proven on both buffers (the checksum is reproduced) |
| 280 to 295 (ECB blocks 35 and 36) | six 16-bit **random-number words** of the save object (the four the game keeps in its control object, and two more), plus two constants that do not change | they are **functions of the wall clock**: the low 16 bits of the Unix time, in seconds, at two clock readings the game makes (frame 2 to 3 and frame 2813), and of the random generator stepped from the first one | proven for the values (each follows from the clock readings of its own run, exactly, in both backends); the code that stores the second reading was not located (section 5) |

**Neither backend is wrong.** In `recomp` both readings fall 4.4 ms and 14.4 ms before a second boundary of the
emulated clock, in `jit` (which reaches the same point 10.8 and 31.4 million ticks later) they fall after it,
so the second count is one higher at both readings; everything else follows. The rule for D52 is in
[the last section](#8-the-rule-for-d52): two ranges, with reasons.

Why 24 bytes when the cause is smaller: the cause is 8 plaintext bytes (in six halfword fields); the checksum
follows from them (2 more bytes change in the plaintext, 10 in all); the cipher spreads those into three
8-byte blocks (two blocks of fields and the checksum's block), 24 ciphertext bytes.

## 1. Method

1. **The buffer.** `--trace-ipc-full` of the recording (`build/golden/G2/ipc.txt`) holds the data of every
   `fs:USER/file` Write. The Write of 81,408 bytes is on the session `fs:USER/file#19` of the trace; its
   request words give the offset (0), the size (0x13e00) and the guest address of the buffer, `0x32f11400`
   (the same address as every other Write of that thread: one buffer is reused). Its CRC-32 is the one N0-28
   reports: `0x4bc7a915` on `recomp` and `0x2cf8030a` on `jit`; both equal the CRC-32 of the file `user1` that
   each run leaves in its user folder.
2. **The dumps.** Three runs of the reference runner (the N0-29 runner, Old 3DS, `TZ=UTC0`, a fresh copy of
   `build/userbase-cfg`, `G2.txt`, `--frames 4925`): recomp twice and jit once, each with `--state-trace` over a
   private observable list (the fields below, read through the game's pointers at every frame) and `--dump`
   of the buffer at the frames around the Write. The runs reproduce the recordings: the `t=` of all 4,926
   frames of each run equals the recording's, and the buffer dumped at game frame 4917 (recomp) and 4918 (jit)
   has the CRC-32 of the Write in each (the buffer holds the slot during frames 4917 and 4918 on recomp, 4918
   and 4919 on jit; before that and after it another payload). `save_probe.py diff` of the two dumps prints exactly the two ranges
   (`0` count 8, `280` count 16).
3. **The layers.** The buffer is not plain: every byte looks random. The game's save code (found from the string
   table: the cipher key string at `0x00e05e04`, referenced by two small functions) puts three layers on a slot,
   which `save_probe.py unwrap` undoes (section 2). With the layers undone the stored checksum equals the
   computed one on both buffers, which is the check that the reading of the layers is right.
4. **Where the plaintext differs, and who writes it.** `unwrap` of both buffers and `diff --align 8` gives the plaintext
   ranges (section 3); the builder of the slot (`0x002abe04`) and the copy it makes (`0x00c2784c`) say which
   object the fields come from; per-frame traces of those fields in each backend say when each one changes;
   the SVC trace says which clock readings the game made, and plain arithmetic on the shared page's date record
   shows that each value is the reading's Unix time modulo 65536 (section 4).

## 2. The three layers of a user slot

Addresses are those of the 1.3.0 code. The builder is called when the save state of the manager is 12
(`0x002ac72c`); it fills the buffer and seals it.

| Step, in the order the game writes | Function | What it does |
|---|---|---|
| fill | `0x002abe04` (builder), `0x00c2784c` | header word 0 gets a format version in its low half; `0x00c2784c` first copies four halfwords from the control object (`g_sGameControlInstance` at `0x00fb6b7c`, +0x20 to +0x26) into the save object (`[0x00fcf7ec]`, +0x16e to +0x174), then copies 0x12600 bytes of the save object (from its +0x5c) to buffer offset 8; three more fills follow at buffer offsets 0x12608, 0x13808 and 0x138c8 (`0x00c1e794`, `0x00c209f0`, `0x00c20a14`) |
| checksum | `0x002abcac` | word 1 of the header (offset 4, little endian) = the 32-bit sum of the **bytes** from offset 8 to the end of the buffer |
| scramble | `0x002ac1c0` | the upper half of word 0 becomes a 16-bit seed (a draw of the game's global random generator); from offset 4, every 16-bit halfword is XORed with the next value of `x -> ((x or 1) * 0xb0) mod 0xff53`, started from the seed (checksum included) |
| encrypt | `0x002ab9b0` | Blowfish, ECB mode, 8-byte blocks read as two little-endian words (the block loop is `0x00d071f0`, the standard Blowfish F function and key schedule at `0x00b70a84`, the key the 56-byte string at `0x00e05e04`), in place |

Loading undoes it in the opposite order (`0x002ab95c`, the block loop `0x00d0711c`, then `0x002abcac` with the
sum compared to the stored one). The tables of the cipher are the standard ones (the tool computes them from
the digits of pi, and the checksum check passes on both buffers with them), so `save_probe.py` needs no game
data but the key: `unwrap FILE --key-from-code build/native-game/code.bin:0x00e05e04`.

Two properties matter for D52:

- **ECB.** Each 8-byte ciphertext block depends on its own plaintext block only: a change in one plaintext byte
  changes exactly the 8 bytes of its block, and nothing else of the buffer.
- **The scramble's seed** (header word 0, upper half) is the same in both backends here, so the other 81,384
  bytes are equal. If a run drew another seed, every byte from offset 4 on would differ and no range mask could
  cover it (section 8).

## 3. The two ranges, in the plaintext

`save_probe.py diff --align 8` of the two unwrapped buffers:

| Plaintext offsets | Block | What | Difference |
|---|---|---|---|
| 4 and 5 | 0 | the low half of the checksum word | the sum differs by exactly the sum of the byte differences below (a difference of 214 in both) |
| 282 and 283 | 35 | halfword: the system random word (control object +0x20, save object +0x16e) | both bytes |
| 284 and 285 | 35 | halfword: the game random word (+0x22, +0x170) | both bytes (always equal to the one before it) |
| 286, 288 | 35, 36 | halfwords: the two other random words (+0x24, +0x26; save object +0x172, +0x174) | the low byte only (+1 in `jit`) |
| 290, 292 | 36 | halfwords at save object +0x176 and +0x178 (the two extra words) | the low byte only (+1 in `jit`) |

The other bytes of blocks 35 and 36 (offsets 280, 281, 294, 295) are equal. Block numbers count from offset 0 in
8-byte blocks: block 35 is offsets 280 to 287, block 36 is 288 to 295, which is why 10 plaintext bytes become 24
ciphertext bytes.

The checksum is the only candidate that fits: the byte sum from offset 8 matches the stored word in both
buffers (`crc-check --algo sum8 --from 8 --stored 4:4`), and crc32, adler32, the 16-bit and 32-bit word sums and
the 32-bit XOR do not.

## 4. Where the six words come from: two clock readings

The game's own clock function (`0x0010c8d8`, reached through `0x0064e83c` and `0x00109fb0`) reads the system
tick (the `svc` at `0x0010ca00`, GetSystemTick) and the date record of the shared page (`0x1FF81000`), and
returns a time. The record is the one the runner fixes: a date of 3,976,257,607,459 ms since 1900 at an update
tick of 2,000,000,000, at 268,111,856 ticks per second, at `0x1FF81040` (the page's counter reads 1; the dumps
of the page at game frames 4916 to 4919 of the three runs hold the same record). The function that turns that
time into a calendar time and back (`0x0064e83c`) yields Unix seconds (the values below show it); the game
keeps their **low 16 bits**.

In the SVC trace the clock function runs three times in the first 4,925 frames, in both backends, always from
the main thread. Unix seconds below are
`floor((date_ms + (tick - 2,000,000,000) * 1000 / 268,111,856) / 1000) - 2,208,988,800`:

| Reading | Game frame | `tick - 2e9`, recomp | `tick - 2e9`, jit | Unix second, recomp | Unix second, jit | Ticks to the next second boundary, recomp / jit |
|---|---|---|---|---|---|---|
| 1 | between frame records 2 and 3 | 143,863,041 | 154,706,363 | 1767268807 | 1767268808 | 1,185,474 (4.4 ms) / 258,454,008 |
| 2 | frame 2813 | 13,546,785,417 | 13,578,195,050 | 1767268857 | 1767268858 | 3,855,898 (14.4 ms) / 240,558,121 |
| 3 | about frame 4430 | 20,954,454,494 | 20,990,412,813 | 1767268885 | 1767268885 | 103,318,789 / 67,360,470 (same second) |

(The emulated date is 2026-01-01 12:00:07.459 at load, so a second boundary falls 541 ms after the load and
every 268,111,856 ticks after that. The first reading of `recomp` is 4.4 ms short of the first boundary and the
first of `jit` 36 ms past it.)

Every value is reproduced exactly:

- **Reading 1 initialises the save object** (`0x00c1d884`, called once from `0x00b7365c` the first time the
  control object's counter reaches 1): it zeroes 0x15204 bytes of the object from +0x5c, writes the **low 16 bits
  of the reading into the six halfwords of the object at +0x16e to +0x178** and copies four of them to the
  control object's random words. The state traces show it: at frame 3 all six halfwords and the four control
  words hold the low 16 bits of the reading's Unix second, one higher on jit than on recomp (the Unix seconds of
  the table above differ by one).
- **The system and game random words are stepped once per frame** (`0x002b0310` and `0x002b0f08`, the same
  recurrence as the scramble's: `x -> ((x or 1) * 176) mod 65363`), except in 4 frames of recomp and 3 of jit
  where they take two steps (all between frames 3071 and 4453, before the restore below, so they do not reach
  the save). At frame 3 they hold one step of the seed. At frame 2814 the save object takes a snapshot of them;
  at frame 4913 (recomp) or 4914 (jit) the control words are set back to that snapshot, and the save is built 4
  frames later (frame 4917 recomp, 4918 jit). So the saved word is **the seed stepped 1 + 2811 + 4 = 2816
  times** in both backends: computed from the low 16 bits of reading 1's Unix second it equals the dumped value
  in both (`step^2811` of the frame-3 value equals the snapshot, and `step^4` of the snapshot equals the saved
  word). Its two bytes differ because the seeds differ by one.
- **Reading 2 sets the save object's +0x176 and +0x178** (the two extra words) at frame 2814, in the same bulk
  update that writes the character's data (the object changes in dozens of ranges between the dumps of frames
  2813 and 2814; the snapshot above and two more halfwords of the +0x16c to +0x17a area change with it). Both
  words hold the low 16 bits of the second reading's Unix second: 1767268857 on recomp and 1767268858 on jit,
  a difference of one, which is the +1 seen at offsets 290 and 292. The 50 between the two readings' values in
  both backends is the 50 seconds of emulated time between frames 3 and 2813.
- **Reading 3** is the same second on both backends and is stored nowhere in this slot.

Therefore every plaintext byte that differs is a function of the Unix second at reading 1 or reading 2, and
the checksum follows from those bytes. The differences between the backends are the emulated clocks: `jit`
reaches frame 3 about 9.0 million ticks (33 ms) later than `recomp` and frame 2813 about 31.4 million ticks
(117 ms) later, and with the date the runner fixes, those delays move both readings across a second boundary.
Nothing in the pair follows from anything but time.

## 5. What this does not establish

- The code that stores reading 2 in the object (a bulk update at frame 2814) was not found statically: the
  readings, the frame and the values are measured and match in both backends, but the instruction that stores
  the halfwords is not named.
- The scramble seed is equal in both backends here. It is a draw of the game's global random generator at
  `0x010e6674`; how many draws came before the save was not traced, so "equal" is an observation of this pair,
  not a derivation that it cannot follow the clock.
- Only G2's `user1` was examined (the spec's scope); the other writes of the session (the 512-byte `system`
  file, the other slots) have equal CRC-32 in the two backends and were not opened.

## 6. The earlier measurement (before N0-29)

Two runs of the previous reference runner (`build/n029/it0/G2/run1` and `run2`) differ in `user1` in the
ciphertext ranges 0 to 7 and 75664 to 75671 (the 16 bytes N0-28 mentions). Unwrapped, the plaintext differs in 1
byte of the checksum word (offset 4) and 3 bytes at offsets 75668 to 75670, in block 9458. So the same layering
explains it: 3 plaintext bytes of one field and the checksum block that follows from them. What the field is
derived from was removed with N0-29's changes (the two runs of `it1` are byte equal, as are the recordings and
the runs here) and this note does not show which of them it was; it shows only where in the slot it is and that
the first 8 bytes are the checksum.

## 7. Repeating it

```
python tools/native/save_probe.py diff A.bin B.bin --align 8                 # the ciphertext ranges
python tools/native/save_probe.py unwrap A.bin --key-from-code build/native-game/code.bin:0x00e05e04 --out A.plain
python tools/native/save_probe.py diff A.plain B.plain --align 8              # the plaintext ranges
python tools/native/save_probe.py crc-check A.plain --algo sum8 --from 8 --stored 4:4
python tools/native/save_probe.py ranges A.bin --zero 0:8 280:16              # equal for both buffers
```

`A.bin` and `B.bin` are the 81,408-byte buffers, from the IPC trace (`--trace-ipc-full`) or from
`--dump f<N>:32f11400:0x13E00` at game frame 4917 (recomp) or 4918 (jit). With the two ranges zeroed, both
dumped buffers have the same CRC-32 (`0xed9ff244`). The per-frame fields were read with an observable list
of lines like `[[0x00fb6b7c]+0x20] 2 c_rnd_sys ignore` (the control object at `[0x00fb6b7c]`, the save object
at `[0x00fcf7ec]`); the clock readings are the `svc 0x28` lines of the SVC trace whose `pc` is `0x0010CA04`.

## 8. The rule for D52

**The difference is not a bug: it is derived from the clock, in two ranges of the written buffer.** The ranges
to put in `config/native/fs_write_masks.txt`, as ranges of the bytes written (after the cipher, which is how a
comparator sees them). Proposed format, one line per range, `#` comments and blank lines allowed:

```
# session  write-size  write-offset  mask-offset  mask-size  reason
G2 81408 0   0 8     block 0 of the user slot: the byte-sum checksum of the plaintext from offset 8; it changes with the fields masked in the next line
G2 81408 0 280 16    blocks 35 and 36 of the user slot: six random-number words, each a function of the Unix second at a clock reading (frame 2 to 3 and frame 2813) and of the generator stepped from it
```

- `session` is the golden session (`G2`); `write-size` and `write-offset` are the size and file offset of the
  Write request the mask applies to (the comparator sees no file name); `mask-offset` is relative to the start
  of the written data. A line that matches no Write is an error (N0-32).
- The reasons say what each range is derived from: a clock reading, stepped by a per-frame generator, plus
  the checksum of those bytes. They are not "random".
- Zeroing these two ranges makes the dumped buffers of the two backends equal over all 81,408 bytes
  (same CRC-32), and no other range of the pair differs. They are three whole ECB blocks, so the mask cannot
  hide anything outside them.

Conditions the rule depends on (for N0-32 and the native runtime):

- The pair's other bytes are equal only while the scramble's seed (header word 0, upper half) is equal. If a
  run draws another seed the whole buffer differs and the comparison has to be done on the unwrapped slot
  instead (`save_probe.py unwrap`: the plaintext differs in the same few bytes and the checksum); that
  would be a decision for D52, not a mask.
- A native runtime whose clock differs from the reference's by more than the 4.4 ms of reading 1 or the 14.4
  ms of reading 2 can cross the same second boundaries and show the same difference; one whose readings fall
  in the same seconds as the reference shows none. The first boundary is 541 ms after the program loads and
  repeats every second, and the reference's two readings fall 4.4 ms and 14.4 ms before one: the margin is
  thin. The mask is the rule; a deterministic clock that reproduces the readings' seconds is an alternative
  that needs no mask.
- Other users of this layer (the 512-byte `system` file and the other slots) were equal in the pair and are
  not masked.
