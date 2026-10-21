# The player camera and its input (USA v1.3.0)

Found statically (October 2026) to let the app's mouse turn the camera directly, as a PC game's mouse look does, instead of pushing the C-stick. Confidence: high for the call chain and the fields, medium for the directions (which way is "left") until checked in the game.

## How the buttons turn the camera

- `sMhPad::update` (`0x00694c98`) turns the C-stick (and the circle pad) into an angle and a length, and the C-stick, the D-pad and the touch screen's camera pad into four **digital** camera actions: up `0x2000`, down `0x1000`, left `0x0800`, right `0x0400` (a stick counts once it is past about 30%: length `>= 0x25` of 127).
- The player's input object (`player+0xe30`) keeps them as bits of `+0x3cc` (`0x00b3cc14` sets them): bit 2 up and bit 4 down, bit 6 left and bit 8 right (bits 1, 3, 5 and 7 are the same directions from the other word). `0x00b0d718(player, n)` tests bit *n*.
- **`sMhCamera::turnByInput` `0x00bfcfd8`** (`this`, `input`, invert flags `sMhCamera+0xe36`) turns the camera's **input angles** by those bits: the pitch at `input+8` and the yaw at `input+0xc`, `u16` angles (`0x10000` = a full turn, sine table at `0x010de258`). Only one axis a frame (up/down first). The speed ramps up while a direction is held: timers at `input+0` (up/down) and `input+4` (left/right) grow by the frame time to a cap, and the step is a maximum times the timer: `0x180` up/down and `0x400` left/right normally, `0xf0`/`0x2e0` and `0x120`/`0x340` in two other modes (`input+0x10` bits 1 and 2). The pitch stays within ±`0x3f00`. Non-inverted, "left" adds to the yaw and "up" to the pitch; flag bits 1 (up/down) and 2 (left/right) of `+0xe36` swap them (the game's camera options).
- **`sMhCamera::updatePlayerCamera` `0x00c0095c`** (from the quest camera update `0x00c06e04`, which `sMhCamera::update` runs in `sGameControl+0x2b` mode 1) calls `turnByInput` with `input = sMhCamera+0xe80`, then the L button's reset (`0x00bff694`: yaw = player angle + `0x8000`) and eases the **rig** at `sMhCamera+0xe40` toward the input angles with `0x00d0cc00(target, now, 3)`: pitch `+0x80` (copy `+0x88`), yaw `+0x84` (copy `+0x8c`). `0x00c056c0` then works out the pitch actually used, and the rig's position and target (`+0x00`, `+0x10`) follow from the angles, the distance `+0x78` and the height `+0x7c`.
- Two other cameras (riding or special scenes; `0x00bfe0cc`, `0x00c015b8`, rig at `sMhCamera+0xf80`, input at `+0xfc0`) call `turnByInput` the same way.

## The port's mouse camera

The hook on `turnByInput` (`port/src/hooks.cpp`, `CameraTurnByInput`) runs the original, then adds the turn the app collected from the mouse (`Port::Hooks::AddCameraTurn`, in degrees) to the input angles; for the main camera (`input == this + 0xe80`) it adds it to the rig's angles too, so that the turn shows at once instead of being eased in. The app uses this while the hook has run in the last 0.3 s (`CameraTurnActive`), and pushes the C-stick by the mouse's speed otherwise (an older code library, or a camera that doesn't take input this way).

To check in the game: which way the mouse turns (the hook's `YAW_SIGN` and `PITCH_SIGN`; the app's *Invert* options flip them meanwhile), whether the village and the other cameras run `turnByInput` too, and whether a mounted or target camera needs more.
