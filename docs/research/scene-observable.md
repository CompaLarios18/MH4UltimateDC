# The game's own scene observable

N0-27 (decision D32). The state traces of the reference runner, the checkpoints of the golden
sessions and N1's whole-game threshold compare **scenes**. This note records how the game's own
value for "the current scene" was found and validated, the value, and what it still cannot do.
Only addresses, offsets, counts, frames and class names are written here: no dump contents, no
disassembly, no screenshot content beyond the neutral words title, creation, village, opening.

All frames are game frames (the `u32` at `0x010571d4`). All measurements are Old 3DS runs
(`--new-3ds off`), `TZ=UTC0`, a fresh copy of the 1.3.0-update user folder with no save, `--cpu recomp`
unless a line says `jit`.

## Result

`scene` is **the class of the topmost game area**: the vtable (a code-region address, the same in
every runtime) of the last `cArea` object in the area table of the `sArea` instance.

```
[[[0x01057298]+0x381c+N+N+N+N]]  4 scene  eq        with N written out as [[0x01057298]+0x3818]
```

(The whole line is in the proposed `observable.txt` below; `N` is repeated four times because the
syntax has no multiplication, and the area count times four is the byte offset of the topmost slot.)

Read it from the inside out:

| Part | Meaning |
|---|---|
| `0x01057298` | the global that holds the `sArea` instance pointer (`sMhArea` is an `sArea` with no fields of its own: the same size, 0x3840) |
| `[0x01057298]+0x3818` = `N` | the number of areas that are alive (1 to 3 in G1, G2 and G6) |
| `[0x01057298]+0x3820 + 4*k` | the area table: eight `cArea*` slots, filled from slot 0 without gaps (`N` equals the number of non-null slots in all 9000 frames of G2) |
| `[0x01057298]+0x381c + 4*N` | the address of the topmost slot (slot `N-1`) |
| the first `[...]` around it | the `cArea*` in that slot |
| the second `[...]` | the `u32` at +0 of the object: its vtable, which names its class |

`N` is never 0 after the first frame (frame 0 reads `-`, before `sArea` exists), and a vtable is never
null, so `scene` is readable from frame 1 on in every run recorded.

### Value to class

The vtable of each class is the one its constructor stores (read in the constructor of the class, found
through its DTI in `config/mt_classes.tsv`). "Seen" means the value occurred in G1, G2 or G6.

| scene value | class (under `cArea`) | seen | where, in G2 (`--cpu recomp`) |
|---|---|---|---|
| `0x00e64d94` | `aInit` | yes | frame 1 |
| `0x00e6c100` | `aGameSys` | yes | frames 2 to 35, and as a short step between areas |
| `0x00e64d5c` | `aDemo` | yes | frames 36 to 1651 (the intro movies and the title), and 4955 to 4956 |
| `0x00e6c138` | `aNewGame` | yes | frames 1652 to 4954 (character creation, both name entries) |
| `0x00e6c170` | `aVillage` | yes | frames 4958 to 5083 |
| `0x00e655c8` | `aQuest` | yes | frames 5086 to 9000 (the opening) |
| `0x00e650bc` | `aArena` | no | |
| `0x00e650f4` | `aEvent` | no | |
| `0x00e6c778` | `aContinue` | no | |
| `0x00e6b50c` | `aViewer` | no | |
| `0x00e5cb38` | `aDLContents` | no | |

The five classes never seen come from their constructors only; N0-30 (the quest sessions) and N1 should
confirm them when a session reaches them. `aInit` is the area that boots the game and hands over to
`aGameSys` one frame later (frames 1 and 2); `aGameSys` is the permanent system area in slot 0.

## Frames of each scene change (`--cpu recomp`)

From `build/scene/<S>-recomp-<n>.txt`, the proposed list, shared runner arguments (no other recorder).

| Session (frames) | Changes: frame, class |
|---|---|
| G1 (1700) | 1 aInit, 2 aGameSys, 36 aDemo |
| G6 (3700) | 1 aInit, 2 aGameSys, 36 aDemo, 1652 aNewGame |
| G2 (9000) | 1 aInit, 2 aGameSys, 36 aDemo, 1652 aNewGame, 4955 aDemo, 4957 aGameSys, 4958 aVillage, 5084 aGameSys, 5086 aQuest |

The steps at 4955, 4957 and 5084 are real states, not noise: the area system pops the creation area
(topmost becomes `aDemo` for 2 frames), then pops `aDemo` (topmost `aGameSys` for 1 frame), then
`aVillage` is pushed; and again `aVillage` is popped (topmost `aGameSys` for 2 frames) before `aQuest`
is pushed. They repeat exactly in both `recomp` runs and in the `jit` run, in the same order.

### Between the runs

| Comparison (G2) | Largest frame delta over the 9 scene changes |
|---|---|
| `recomp` run 1 against `recomp` run 2 | **0** (every change on the same frame; the traces differ in other values, as the runner is not yet exact) |
| `jit` against `recomp` run 1 | **3** (changes at frames 5084 and 5086 come at 5087 and 5089 in `jit`; the others are 0 to 2 frames later: 36 to 38, 1652 to 1653, 4955 to 4956, 4957 to 4958, 4958 to 4959) |

`validate --tolerance 2` gates only runs of one backend, as the spec says. The `jit` against `recomp`
difference is not a property of the observable (the area switches themselves happen on other frames in
the two timing models); it is a number N0-28 needs: against D33's threshold of 2 frames, the changes at
5084 and 5086 are 3 frames apart.

### Frames for `--expect-distinct` (G2)

One frame inside each scene found on the screenshots (`--shots`, every 2000 ms, mapped to frames with the
state trace's ticks): **1500** (the title, class `aDemo`), **3000** (creation, `aNewGame`), **5000**
(the village class, `aVillage`; the screens are blank at that moment) and **5500** (the opening,
`aQuest`). The four values are pairwise distinct in all three G2 traces.

```
python tools/native/scene_probe.py validate --observable build/scene/observable.txt --tolerance 2 \
    --expect-distinct G2:1500,3000,5000,5500 \
    build/scene/G2-recomp-1.txt build/scene/G2-recomp-2.txt build/scene/G2-jit-1.txt \
    build/scene/G1-recomp-1.txt build/scene/G6-recomp-1.txt
```
exits 0: every trace reads `scene` from frame 1 on; G2 has 6 distinct values in each trace; the sequence of
values is the same in the three G2 traces; the two `recomp` runs agree to 0 frames.

## How it was found

1. **Static search.** `sMhMain::createProperty` (`0x00af46c8`) registers the names of its subsystem
   pointers (`mpRender`, `mpScene`, `mpShader`, `mpUnit`, `mpTool`, `mpArea`, `mpResource`, ...): `mpArea` is the field
   at `sMhMain+0x2ac`, `mpScene` at `+0x2b4`. The `sArea` constructor (`0x0064307c`, called from the
   `sMhArea` constructor `0x00af4380`) stores `this` in the global `0x01057298`, builds an array of 0x200
   entries of 0x1c bytes (`sArea::AreaInfo`) from `+0x14` (an array-constructor helper called with that
   count and element size), clears the words from `+0x3814` to `+0x383c`, and its destructor walks
   eight pointers from `+0x3820` and releases each: the area table. A property function of `sArea`
   (`0x00642b38`) registers `mpArea` and `mAreaPt`, the latter a field at `+0x3818`; by its value
   (1 to 3, equal to the number of non-null slots in every frame of G2) it is the area count.
   The area classes' constructors (found from their DTIs in `config/mt_classes.tsv`) store the vtables of the
   table above.
2. **Candidate expressions in a state trace.** G1 and then G2 with a state trace of about 60 candidates
   (`--observable`): for each of the eight slots the object pointer and the vtable, the words at
   `+0x3800 .. +0x381c`, the first words of the objects in slots 0 and 1, `game_mode` and the existing
   observables. A state trace is read once per frame and writes nothing to the guest, so the structure came out of one G2 run.
3. **Dumps and `rank`.** In the same run `scene_probe.py dumps` generated `--dump` options for `sArea`
   (0x3840 bytes), `sMhMain` (0x358) and `sGameControl` (0x41d0) every 100 frames; `scene_probe.py rank`
   with the phases 36, 1352, 1652, 4958, 5086 (the frames the state trace had found) listed the changing
   fields: 386 of 56182 fields scored above zero. At 100 frames per dump it confirmed the count and the
   slots (fields at `+0x3818`, `+0x3824`, `+0x3828` of `sArea`; the mode byte of `sGameControl`), and
   listed fields of `sGameControl` that change once near frame 1700 (an array of 0x1c-byte entries) and
   others that were not examined one by one; the `sMhMain` fields that change do so at almost every dump
   (counters and timers) or without a pattern near the phases. Dumps are coarse for this job (a change is
   dated to the 100 frames before the dump that shows it); the state trace with a chain showed the area
   table at every frame.
4. **Validation.** The expression was checked in the C++ reader of the runner against the slots' own
   columns (it gave `slot[N-1]` on every frame of G6 to 3700 and of G2), then the five traces above were
   recorded with the proposed list.

## Candidates, and why each was rejected

| Candidate | What it does in G2 | Why it is not `scene` |
|---|---|---|
| `[[0x0104dea0]+0x30]`, the `sApplet` state | 0 in every frame (D32) | never changes; 4 to 6 mean HOME and applet overlays |
| `sGameControl` `game_mode` (`[[0x00fb6b7c]+0x2b]`; the global holds a pointer) | 0 from frame 1 to 4958, 2 from 4959, 6 from 5084, 1 from 5087 | the same value for the intro, the title and creation |
| the class in table slot 1, the "main area" (`[[[0x01057298]+0x3824]]`) | `aDemo` 36 to 4956 (intro, title and creation alike), null for 1 frame at 4957 and 2 frames at 5084 | does not separate title from creation, and reads `-` between areas |
| the class in table slot 2 | `aNewGame` 1652 to 4954 only | `-` outside creation |
| the state word of `aGameSys` (slot 0 object, +4) | low byte 0 or 1 while booting, 2 from frame 35 to 4955 (intro, title and creation), 8 village, 9 quest | title and creation share a value; the boot values change every few frames |
| the area count `N` | 1, 2, 3, 2, 1, 2, 1, 2 | not an id: title, village and quest all read 2 |
| the state word of the object in slot 1 (+4) | steps through the intro, title and creation, then means something else for `aVillage` and `aQuest` | one class's step, not a scene; values collide across classes |
| heap addresses (slot objects, the instance pointers) | differ between objects and could differ between runs and runtimes | a heap address is not a scene id |
| the topmost area class (chosen) | the table above | passes `validate` |

## What the observable cannot do

- It names areas, not what happens inside one: the intro movies, the title and the menu are all
  `aDemo`; the whole opening (and, we expect, a whole quest) is `aQuest`. A finer scene would need the
  state word inside the area object (for `aDemo` the word at +4 steps through intro, title and creation),
  which is class-specific.
- During an area switch it shows the area underneath for 1 to 3 frames (`aDemo` or `aGameSys`). Those
  steps are part of the recorded sequence (they were identical in the three G2 traces); what a comparison
  does with such a short step is for N0-09 and N0-25, not for the observable.
- Areas stacked on another one (creation over `aDemo`) hide the one below: the topmost area wins.
- The five classes never seen (above) are unverified, and an area of a class not in the table is shown by
  its vtable all the same.
- The 3-frame difference between `jit` and `recomp` at the last two changes of G2 is for N0-28 (D33).

## Proposed lines (the orchestrator edits `config/native/observable.txt`)

`scene` becomes the chain above, the sApplet byte stays as `applet_state` (eq), the rest is unchanged. The
file used for the five traces is `build/scene/observable.txt`:

```
[[[0x01057298]+0x381c+[[0x01057298]+0x3818]+[[0x01057298]+0x3818]+[[0x01057298]+0x3818]+[[0x01057298]+0x3818]]]  4 scene               eq
[[0x0104dea0]+0x30]  1 applet_state        eq
0x010571d4           4 frame_counter       eq
[[0x01057194]+0x5c]  4 smain_delta         ignore
[[0x00edbe90]+0x9788+0x8c] 2 quest_id      eq
[[0x00fb7190]+0x42c] 4 stage_loader_state  eq
```

and, with a comment line explaining the chain, for `config/symbols.txt`:

```
0x01057298 g_sAreaInstance             # sMhArea*: area count at +0x3818, eight cArea* slots at +0x3820 (topmost = slot count-1)
```

## Repeating it

```
python tools/native/scene_probe.py dumps --session build/scene/sessions/G2.txt \
    --object 0x081cce60:0x3840 --every 100         # runner options; the object address is the value of 0x01057298
python tools/native/scene_probe.py rank build/scene/dumps --phases 36,1352,1652,4958,5086
python tools/native/scene_probe.py validate --observable build/scene/observable.txt ...   # as above
```

`mh4u_runner` is run from the repository root (the default `--observable` path is relative) with
`--state-trace build/scene/<S>-<cpu>-<n>.txt --observable build/scene/observable.txt`, the session as
`--input-file` and its `# frames:` total as `--frames`. `--shot-every` takes milliseconds, not frames: a
value written as `f100` is not read as frames and writes a screenshot per millisecond (19 GB in four minutes).
