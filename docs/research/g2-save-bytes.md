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
| 280 to 295 (ECB blocks 35 and 36) | six 16-bit words of the save object (the four the game keeps in its control object, and two more), plus two constants that do not change: two of the words are the game's random generator stepped from the first clock reading's seed, four are the low 16 bits of the Unix second at the two readings | they are **functions of the wall clock**: the low 16 bits of the Unix time, in seconds, at two clock readings the game makes (frame 2 to 3 and frame 2813), and the generator stepped from the first one | proven for the values (each follows from the clock readings of its own run, exactly, in both backends); the code that stores the second reading was not located (section 5) |

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
- **Reading 3** is the same second on both backends. (N0-34, section 9: it is stored in this slot after
  all, in the hunter's creation stamp at plaintext offsets 75668 to 75675, together with the frame counter
  and part of the console's ID; equal on both backends here, so it caused no difference in this pair.)

Therefore every plaintext byte that differs is a function of the Unix second at reading 1 or reading 2, and
the checksum follows from those bytes. The differences between the backends are the emulated clocks: `jit`
reaches frame 3 about 9.0 million ticks (33 ms) later than `recomp` and frame 2813 about 31.4 million ticks
(117 ms) later (the 9.0 million is the frame-3 record of the state trace; the first clock reading itself, a
little before that record, is 10.8 million ticks later, as the table above says), and with the date the runner
fixes, those delays move both readings across a second boundary.
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
the first 8 bytes are the checksum. (N0-34 answered it, section 9.4: the three bytes are part of the console's
ID, which that runner drew anew in every run.)

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

## 9. The byte that only the native runtime shows (N0-34)

N1-21 ran G2 on the native runtime and compared its `user1` with the recording. Outside the two ranges of
section 8 the slots differ in one more byte, plaintext offset 75674 (cipher block 9459, offsets 75672 to
75679 of the written data). A second native run made for this work (same runner, same user-folder base,
same session) differs there in two bytes, 75674 and 75675. This section says what they are. Nothing in
it is a defect of the native runtime; the rule is the third line of the mask file, at the end.

### 9.1 What the slot holds at 75668 to 75675

The first fill after the 0x12600-byte block (section 2) is `0x00c1e794`: it copies the 0x1200-byte record
of the save object at `[0x00fcf7ec] + 0x85a60 + slot * 0x1200` to buffer offset 0x12608 (75272). The slot
is the byte at control object +0xa4; in G2 it is slot 0, so offset 75272 is the start of that record and
the record's offset 0x18c is plaintext offset 75668. The 8 bytes at record offsets 0x18c to 0x193 (75668 to
75675) are a **creation stamp** of the hunter: the record is cleared and filled by the builder at
`0x00c22d9c` (called from `0x00b778ec`, when the character is created; besides the stamp it copies a name
and sets a few header halfwords and flags). The record's offsets 0x184 to 0x18b hold another 8-byte field
that no run changes (it is equal in every slot of this note, the pre-N0-29 pair included); a debug function
of the game (`0x0020efb0`) prints the stamp as 16 hex digits.

The builder gathers three inputs in this order, which is why they all belong to one moment:

1. the clock function `0x0064e83c` (the third clock reading of section 4, the third `svc 0x28` at `pc`
   `0x0010CA04`), converted to a calendar time (`0x0064e8d8`, UTC under `TZ=UTC0`);
2. the control object's counter at +0x18 (the state trace's `c_sys_timer` of section 4's observable list;
   it counts the same frames as the `frame_counter` at `0x010571d4` but is incremented a little before
   that one, so the builder reads the number of the next frame record);
3. the reply of `cfg:u` command 3 with the game's unique ID 0x1263 as parameter (`0x00483bd4`), 7,241
   ticks after the clock reading in every run (an IPC the SVC trace and the IPC trace both show).
   This is `GetTransferableId`: the last 8 bytes of SHA-256 over the console's unique ID (8 bytes) and
   the 20-bit title salt (`Azahar src/core/hle/service/cfg/cfg.cpp`, line 526).

| Stamp byte | Plaintext offset | Value |
|---|---|---|
| 0 | 75668 | bits 24 to 31 of the 64-bit reply (the top byte of its low word) |
| 1 | 75669 | bits 32 to 39 (the low byte of its high word) |
| 2 | 75670 | bits 40 to 47 |
| 3 | 75671 | (calendar year - 1900 + 0x6c) and 0xff |
| 4 | 75672 | (month number << 4) + ((day and 0x1e) >> 1) |
| 5 | 75673 | (minute and 0x1f) + (day << 5), modulo 256 |
| 6 | 75674 | (second and 0x3f) + ((frame and 0x300) >> 2), where frame is the counter of item 2 |
| 7 | 75675 | frame and 0xff |

`save_probe.py stamp` builds those 8 bytes from the three inputs and says, byte by byte, whether the slot
holds them (it never prints a byte).

### 9.2 The diffs, and the byte

`save_probe.py unwrap` of the recording, of the saved native slot of N1-21 (`native 1`) and of the slot of
the second native run (`native 2`), all three with the checksum matching, then `diff --align 8`:

| Pair | Plaintext ranges (offset, count) | Written-data ranges (offset, count) |
|---|---|---|
| native 1 against the recording | 4, 2; 282, 5; 288, 1; 290, 1; 292, 1; **75674, 1** | 0, 8; 280, 16; **75672, 8** |
| native 2 against the recording | 4, 2; 282, 5; 288, 1; 290, 1; 292, 1; **75674, 2** | 0, 8; 280, 16; **75672, 8** |
| native 2 against native 1 | 4, 1; 75675, 1 | 0, 8; 75672, 8 |

Inside the two ranges of section 8 (offsets 4 and 5, which are the checksum, and 282 to 293) the native
slots differ for the reason of section 4, and the same model reproduces them: the system and game random
words are the generator stepped 2816 times from the low 16 bits of the Unix second at reading 1, two more
words are that seed, and two are the low 16 bits of the Unix second at reading 2 (`save_probe.py
halfword-check`; the 2816 steps are the same in the native runs as in the recording). The native
clock gives the readings 1 and 2 the seconds 1767268800 and 1767268849 against the recording's 1767268807
and 1767268857, and everything follows. Outside those ranges, **byte 75674 is stamp byte 6: the second of
the third clock reading, with two bits of the frame counter**, and 75675 is stamp byte 7, the low byte of
that counter.

### 9.3 Proof: every stamp is reproduced

Inputs are the third clock reading's `GetSystemTick` result (the `out` words of the SVC trace at `pc`
`0x0010CA04`), the shared page's date record of the run, the control counter (the state trace's next
frame record after the reading, which is the value the builder reads), and the reply words of `cfg:u`
command 3 from the IPC trace. The reply words are derived from the console's unique ID and stay in
`build/`. Date record: the reference runner's is 3,976,257,607,459 ms since 1900 at update tick 2,000,000,000
(section 4); the native runtime's is the `--init-time` default, 1767268800, at tick 0
(`SystemPages::UpdateSharedPage`), i.e. 3,976,257,600,000 ms since 1900.

| Run | Tick of reading 3 | Date record, update tick | Unix second | Calendar (UTC) | Counter | `stamp` |
|---|---|---|---|---|---|---|
| recording (and `recomp` repeat) | 22,954,454,494 | 3976257607459, 2e9 | 1767268885 | 2026-01-01 12:01:25 | 4453 | all 8 bytes match |
| native 1 | 20,635,906,756 | 3976257600000, 0 | 1767268876 | 2026-01-01 12:01:16 | 4453 | all 8 bytes match |
| native 2 | 20,636,227,444 | 3976257600000, 0 | 1767268876 | 2026-01-01 12:01:16 | 4452 | all 8 bytes match |
| pre-N0-29 pair, run 1 and run 2 | 22,954,454,494 | as the recording | 1767268885 | as the recording | 4453 | all 8 bytes match, each with its own reply words |

(The ticks are the SVC trace's first two `out` words, the low word first: the recording's are
`0x58312DDE` and 5. The `recomp` repeat made for this work has the recording's state trace, and the dump of
the slot at game frame 4453 holds the stamp; at frame 4452 it holds nothing.) `save_probe.py clock-second`
gives the Unix second of each line from its tick and date record.

The seconds differ by 9: 25 in the recording and 16 in the native runs, which is the whole difference of
75674 (and the checksum). The 9 seconds are two things, both clock:

- the date at the start. The reference runner starts at 12:00:07.459 (its fixed date record, section 4);
  the native runtime starts at 12:00:00.000 (`--init-time`, whose text in `docs/native/runner.md` calls it
  the fixed start of the reference runs, but the reference's date record is 7.459 s later). That is 7.459 s;
- the time the game takes to reach the character's creation: 78.156 s of emulated time in the recording and
  76.968 s in the native runs (the native runtime reaches the same game frame 1.19 s earlier; N1-21 measured
  the scene changes at +8 and -3 frames).

7.459 + 1.188 = 8.647 s, which moves the floor of the second from 76 to 85 (the recording's reading falls
615 ms into its second, the native readings 968 and 969 ms into theirs).

Byte 75675 differs only between the two native runs: the control counter at the call was 4453 in the
recording and in native 1 and 4452 in native 2. The creation comes a few frames after the scripted A press
at game frame 4450 (the likely trigger; not traced), and the native runs are not cycle-exact, so the
call's phase against the frame boundary moves by milliseconds: the call fell 2.4 ms (recording) and 1.0 ms
(native 1) after the record of frame 4452 and 3.8 ms before it in native 2 (the records are the state
trace's `t=`). The call is within a few milliseconds of a boundary in all three, so one frame more or less
is expected from run to run.

### 9.4 The earlier pair (section 6)

The 3 bytes at 75668 to 75670 that differed between the two runs of the previous reference runner are stamp
bytes 0 to 2: bits 24 to 47 of the `cfg:u` reply, which that runner drew anew on every run (the console's
unique ID of the config savegame it made; N0-29's runner fixes it). The two runs' replies differ in the IPC
traces (`build/n029/it0/G2/run1` and `run2`), and each slot's stamp is reproduced from its own reply (the
last row of the table above). The first 8 bytes of the slot (the checksum) differ only because of this. So
section 6's open question is answered, and **section 4's last bullet is wrong**: reading 3 is stored in
this slot, in the stamp. It is the same second in both backends of N0-28 (so it caused no difference there).

### 9.5 The rule

**The byte is derived from the clock and the frame counter; it is not a defect of the native runtime.** The
range to add to `config/native/fs_write_masks.txt` (the orchestrator edits it) is one whole cipher block:

```
G2 81408 0 75672 8   block 9459 of the user slot (user1): stamp bytes 4 to 7 of the hunter's creation stamp (month and day, minute and day, the second with two bits of the frame counter, the low byte of the frame counter): functions of the third clock reading's calendar time and of the control counter at the call (frame 4452 to 4453)
```

With it, the written bytes of the native runs equal the recording's in all 81,408 bytes but the three
masked ranges (`save_probe.py ranges --zero 0:8 280:16 75672:8` gives one and the same CRC-32 for the
recording and the two native slots). The range hides at most those 8 bytes; stamp bytes 4 to 7 are the
only ones of the block that the clock and the frame counter move, and the block's other 4 bytes were equal
in every run.

Conditions, as in section 8:

- Stamp bytes 0 to 2 (block 9458, offsets 75664 to 75671) are equal only while the console's unique ID is
  the same as the recording's. The golden check copies `build/userbase-cfg`, which holds a config savegame,
  so it is; a native run with a user folder that has none makes a random ID (D37) and then block 9458 differs
  too. That is a second range for such a run, not for the golden check, and it is not proposed.
- The native readings fall 31 to 33 ms before the next second boundary (the recording's, 385 ms), so a
  native run a little slower than these two shows the next second (two frames are enough), and the frame
  counter moves by one as 9.3 shows. A narrower range than the block would not hold; the minute (stamp
  byte 5) is 44 s away from changing. The cleaner alternative is the one of section 8, a deterministic
  clock: it would need the reference's 7.459 s in the date record **and** the game reaching frame 4453 at
  the same tick as the recording (the 1.19 s above, a timing-model question of the native runtime), so it is
  not proposed here.
- The stamp is written once, at the character's creation. If G2 changes so that the character is created
  elsewhere, or a second hunter is created, the offsets move.

### 9.6 Repeating it

```
python tools/native/save_probe.py unwrap native.bin --key-from-code build/native-game/code.bin:0x00e05e04 --out native.plain
python tools/native/save_probe.py diff native.plain rec.plain --align 8          # the plaintext ranges of 9.2
python tools/native/save_probe.py diff native.bin rec.bin --align 8              # the written-data ranges
python tools/native/save_probe.py clock-second --date-ms D --update-tick U --tick T
python tools/native/save_probe.py stamp native.plain --at 75668 --id-low W --id-high W --unix-second N --frame N
python tools/native/save_probe.py halfword-check native.plain --at 282 --lcg-seed S --steps 2816
python tools/native/save_probe.py ranges native.bin --zero 0:8 280:16 75672:8     # equal for the recording and both native slots
```

`native.bin` is `title/00040000/00126300/data/00000001/user1` of the user folder after a native run of G2
(`mh4u_native_runner` on the native game folder with the code library, `--user` a copy of the user-folder
base, `--input-file tools/native/golden/G2.txt`, `--frames 5000` is enough: the save is written at about
frame 4917). The clock readings are the `svc 0x28` lines of its SVC trace (`--trace-svc`) whose `pc` is
`0x0010CA04` (the third one; the tick is the `out` words, high word second in the line), `W` and `W` the two
reply words of the `cfg:u` command 3 line with parameter 0x1263 in `--trace-ipc`, and the counter the
`c_sys_timer` of the next frame record after the clock reading (`--state-trace` with an observable list
that has `[[0x00fb6b7c]+0x18] 4 c_sys_timer ignore`). The `recomp` repeat is the run of section 1 with
`--frames 4456`, a state trace of the slot's words (`[[0x00fcf7ec]+0x85be4]` for the first four) and `--dump`
of the heap at frames 4452 and 4453. `D` and `U` are the shared page's date record and update tick (section 9.3).
