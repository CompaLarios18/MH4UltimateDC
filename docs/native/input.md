# Input in `mh4u-native` (N3-07)

The input the Azahar app gives the game, on SDL3 and without Azahar's input engine: the keyboard, the mouse's buttons and wheel and a controller on every 3DS button at once, a controller mapped by itself as in `mh4u`, the keyboard-and-mouse mode with the mouse as the camera, the C-stick and ZL/ZR for the Circle Pad Pro (N3-08), and the in-game menu's mute, as one `mh4n::InputState` for the live input source (`native/src/input_live/`, N2b-08).

The code is `native/app/input/` (the module `mh4n_app_input`, `mh4n_app_module(... DEPENDS mh4n_app_shared mh4n_app_host)`), its tests are `native/app/tests/app_input_test.cpp`. It builds on the app's shared sources, which `native/app/shared` compiles once (`port/app/config.cpp`, `input.cpp`, `kbm.cpp`): the settings (`App::Config`), the key names (`Input::KeyCode`, `KeyCodes`) and the mouse arithmetic (`Kbm::MouseCamera`, `ItemScroll`, `Pulses`). It includes no Azahar and no SDL2 header; the SDL3 gamepad layer is the only file that includes SDL3 (`gamepads_sdl3.cpp`).

The wiring (the window's events, the shell's loop, `input_live::Find(rt)->Set`, the hooks' camera) is **N3-14's**, not this package's.

## The interface (`input/input_mapper.h`)

`mh4n::app::InputMapper` (main thread only):

| Call | What it does |
|---|---|
| `InputMapper(IGamepadLayer*)` | the controllers come through `IGamepadLayer::List()` (`PadInfo`: the instance id the window's events carry, the SDL GUID string, the port among controllers of that GUID, the name). `MakeSdlGamepads()` is the SDL3 layer (it opens every gamepad it lists: SDL3 sends a gamepad's events only to the program that opened it). A test brings a fake |
| `Configure(const App::Config&)` | reads the settings, asks the layer, picks the controller, logs the controller line, builds the bindings. Call again when the settings change: what is held stays held, and so does the mode the player is in |
| `bool OnEvent(const App::InputEvent&)` | one event of the window that the in-game menu did not take. `true`: it belongs to input (keys, controller events, a mouse event while the mouse is the camera) and the shell does not use it; `false`: the shell's (the app's hotkeys, the window's own events, and the mouse's events while it is the stylus: those are touches) |
| `void Tick(double now_s)` | once per loop, after the events, on a clock that only goes forward |
| `InputState State() const`, `on_state` | what the game reads; `on_state(const InputState&)` is called when it changed (after an event, `Tick`, `SetMuted` or `Configure`) |
| `SetMuted(bool)` | the menu is up |
| `bool MouseIsStylus() const`, `bool WantsRelativeMouse() const` | the mouse is the touch screen's stylus / the camera. The complement of each other; refreshed by `Tick` and `SetMuted` |
| `SetCameraTurnActive(bool)`, `on_camera_turn(yaw, pitch)` | whether the port's camera hook runs is the runtime's to say (N3-14; this class never asks `Port::Hooks`); while it does, the mouse's motion goes to `on_camera_turn` and pushes no stick |

`InputState`'s touch fields are never set here: the shell's touch tracker owns them (`LiveInput::Set` ignores them). There are no motion sensors (as `mh4u`) and no runner options, so nothing is proposed for `runner.md`.

## Buttons

A 3DS button reads pressed while **any** of these is: one of its keys (`Input::KeyCodes(Config::keys[button])`: keyboard keys, `Mouse Left` and the like, `Wheel Up` and the like), the item bar's macro code (`Input::MACRO_KEY + button`, which the wheel's L+Y / L+A presses), or its controller bind. The same joins as `azahar_input.cpp`'s `BuildInputProfile` and `AnyButton`.

`Config::button_params[button]`, when it is not empty, **replaces** all three (as Azahar's profile does): a key, the macro code and the default pad bind of that button are gone. What is read of the parameter strings (Azahar's `ParamPackage` format, `azahar_rules.h`):

| Parameters | Meaning |
|---|---|
| `engine:keyboard,code:N` | key code `N` (an SDL scancode, or one of `input.h`'s codes) |
| `engine:sdl,api:controller,button:N` | controller button `N` (SDL2's numbers, which SDL3 keeps) |
| `engine:sdl,api:controller,axis:N,direction:+/-,threshold:T` | axis `N` past `T` (value over 32767; default threshold 0.5, default direction `+`) |
| `engine:sdl,hat:0,direction:up/down/left/right` | the D-pad (the first hat of the device) |
| `guid`, `port`, `maptype` | which controllers read it, as Azahar's SDL engine picks them: `maptype:guid+port` (the default) with a `port` that exists reads that controller alone, otherwise any controller of the `guid` (empty or `0`: any controller); `maptype:guid` any port; `maptype:all` any controller |

Anything else is **logged once and ignored** (the button then never reads pressed): another engine (`gcpad`, `analog_from_button`, ...), and an `engine:sdl` button or axis that is not `api:controller` (a raw joystick's numbering is not what `App::InputEvent` carries: only gamepad events reach the mapper), and a hat other than 0. "Once" means once per process, however often the settings are read.

Only the 3DS buttons `InputState` has are read: A, B, X, Y, the D-pad, L, R, Start, Select, ZL, ZR. HOME, Power, Debug and Gpio14 have no field.

## The controller

The **first** gamepad the layer lists (`Config::controller_auto`; `controller = off` reads none) is the one the default binds read. The mapper keeps the state of every gamepad (for `button_params` that name another), but the default binds read only the picked one. When a gamepad is added or removed (`PadAdded`, `PadRemoved`) the layer is asked again, the first is picked again, the bindings are rebuilt, and the controller line that `port/app/input.cpp` writes is logged: `controller: <name> (port <n>)` (`controller` when it has no name), `no controller found`, or `controller off`. A removed gamepad's buttons and sticks read released and centred at once.

### The default binds

Azahar's `xinput_to_3ds_mapping` (the layout for an Xbox style pad; Azahar's Nintendo layout is only chosen when the 3DS's A was pressed on the controller's A while mapping, which the app's profile never does), with the trigger branches of `SDLState::GetSDLControllerButtonBind` and the stick axes of `GetSDLControllerAnalogBindByGUID`. SDL3 names the face buttons by position; the numbers are the same as SDL2's, which is what `InputEvent::button` carries (`app_input_test` checks it with `static_assert`s against SDL3's own enumerations).

| 3DS | SDL2's name | SDL3's name | n |
|---|---|---|---|
| A | `SDL_CONTROLLER_BUTTON_B` (right) | `SDL_GAMEPAD_BUTTON_EAST` | 1 |
| B | `..._A` (bottom) | `..._SOUTH` | 0 |
| X | `..._Y` (top) | `..._NORTH` | 3 |
| Y | `..._X` (left) | `..._WEST` | 2 |
| Up, Down, Left, Right | `..._DPAD_*` | `..._DPAD_*` | 11, 12, 13, 14 |
| L, R | `..._LEFTSHOULDER`, `..._RIGHTSHOULDER` | `..._LEFT_SHOULDER`, `..._RIGHT_SHOULDER` | 9, 10 |
| Start | `..._START` | `..._START` | 6 |
| Select | `..._BACK` | `..._BACK` | 4 |
| ZL, ZR | `SDL_CONTROLLER_AXIS_TRIGGERLEFT`, `..._TRIGGERRIGHT` past 0.5 | `SDL_GAMEPAD_AXIS_LEFT_TRIGGER`, `..._RIGHT_TRIGGER` | axes 4, 5 |
| circle pad | `AXIS_LEFTX`, `AXIS_LEFTY` | `..._LEFTX`, `..._LEFTY` | axes 0, 1 |
| C-stick | `AXIS_RIGHTX`, `AXIS_RIGHTY` | `..._RIGHTX`, `..._RIGHTY` | axes 2, 3 |
| (HOME) | `SDL_CONTROLLER_BUTTON_GUIDE` | `SDL_GAMEPAD_BUTTON_GUIDE` | 5: **not bound**, the Guide button opens the app's menu |

The stick clicks, the paddles and the touchpad have no bind (the right stick's click is the shell's touch-screen switch, `window.cpp`).

## Sticks

For each of the circle pad and the C-stick the mapper evaluates, in this order: the stick's keys (`Config::stick_keys`, `up,down,left,right[,halving key]`), the picked controller's stick, and, for the C-stick, the mouse's push. **The furthest push wins** (`AnyAnalog`: a later source replaces an earlier only if it is strictly longer). Details, from Azahar's SDL engine and its "analog from button" device:

- a controller's stick: each axis over 32767, y turned over (SDL's y grows downwards, the 3DS's upwards), a point outside the unit circle cut back onto it, then the dead zone if the parameters name one (`stick_params`' `deadzone`, 0..0.99; the default binds have none);
- keys: x and y are -1, 0 or +1; a diagonal is multiplied by 0.7071; the halving key multiplies by 0.5 (a fifth key in `stick_keys`; the default `W,S,A,D` and `O,L,K,;` have none);
- **the raw value** is `roundf(x * 0x9A)` (Azahar's `hid.cpp`: `MAX_CIRCLEPAD_POS = 0x9A`, "rounded rather than truncated on actual hardware"; `native/src/input_script` uses the same): ±1.0 reads ±154, ±0.5 reads ±77, up is positive. The circle pad's averaging over several reads is the HID module's (`native/src/services/hid`), not applied here;
- `Config::stick_params[stick]`, when it is not empty, replaces the keys, the controller and the mouse's push: `engine:sdl,api:controller,axis_x:N,axis_y:N[,deadzone:D]` with the `guid`/`port`/`maptype` of the table above is read; any other engine (including `keyboard`, which Azahar has no analog device for, and `analog_from_button`, which the Qt frontend writes for keyboard sticks) is logged once and ignored, and the stick reads centred.

## The mouse and the keyboard-and-mouse mode

The rules are those of `port/app/window.cpp`, decided over events instead of SDL2 calls.

**Mode** (`Config::input_mode`): `keyboard_mouse` is always on, `controller` never, `auto`: the device used last. Auto starts with the keyboard and mouse when there is no controller at the first `Configure` (and with the controller's way when there is one); a later `Configure` keeps the mode the player is in. The last device changes on:

- a controller's button pressed: the controller's way; a stick pushed past 16000 (of 32767): the controller's way (past 8000 only marks the echo window below);
- a key pressed (not a repeat) and not within 150 ms of a controller event (Steam Input's desktop layouts send keys for a controller's buttons and sticks; the time is `Tick`'s clock, so within the loop's resolution) and not Escape, the fullscreen key, the frame-counter key, the key-guides key or a stylus key: the keyboard and mouse's way;
- a mouse button pressed while the mouse is the camera: the keyboard and mouse's way.

Moving the mouse alone changes nothing (as in `window.cpp`).

**The camera** (`WantsRelativeMouse()`): the mode is on, the window has the focus, no stylus key (`Config::stylus_key`, `Left Alt`) is held, and the menu is not up (`SetMuted(false)`). The focus is not an event of `App::InputEvent`: the mapper knows `FocusLost` (it lets go of every key, as `window.cpp` does) and takes the focus back at the next key or mouse button pressed. The shell sets SDL's relative mouse mode from `WantsRelativeMouse()` and lets go of a touch when it turns on. When the camera ends, the mouse's five buttons are let go and the push on the C-stick is centred at once.

While the mouse is the camera: a motion pushes the C-stick by how fast the mouse moves (`Kbm::MouseCamera`: smoothed, 1500 px/s at sensitivity 50 pushes all the way, the dead zone of `mouse_deadzone`, `mouse_vertical`, `mouse_invert_y`), or, with `SetCameraTurnActive(true)`, calls `on_camera_turn(yaw, pitch)` in degrees (`mouse_sensitivity * 0.0024` a count, vertical share, both inversions) and pushes nothing. Mouse buttons are keys (`Mouse Left`..`Mouse 5`, `Config::keys` names them). The wheel (one call per event and axis): if the code is in `item_prev` the item bar scrolls left (L held, then Y pressed and let go, L let go, three frames a step, at most five scrolls waiting), `item_next` right (A); any other wheel code is pressed for four frames as a key (`Wheel Right` on a button). While the mouse is the stylus (the controller's way, the stylus key held, the menu up), its events return `false`: the shell reads them as touches.

**The app's hotkeys** (`window.cpp`'s `OnKey`): the fullscreen key, Alt+Enter, the frame-counter key, the touch-screen keys (`touch_screen_key`) and the key-guides key are the shell's: `OnEvent` returns `false` and the key is never pressed for the game. (A key let go is always let go.)

## The mute

As `AnyButton` and `AnyAnalog`: while muted every button reads released and every stick centred; once unmuted, a button still held reads released until it is let go, a stick still pushed (past a quarter of its range) reads centred until it comes back. The state is per button and per stick and is updated whenever the bindings are read (every event, `Tick`, `SetMuted`, `Configure`).

## Derived from Azahar (D6)

`azahar_rules.cpp` and `app_input_test.cpp` carry the provenance header (GPL-2.0-or-later; all five sources say "Licensed under GPLv2 or any later version"), at the pinned commit `5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6`:

| Rule | Azahar source |
|---|---|
| the default binds, the trigger and stick axes | `src/input_common/sdl/sdl_impl.cpp`: `xinput_to_3ds_mapping`, `SDLState::GetSDLControllerButtonBind`, `GetSDLControllerAnalogBindByGUID` |
| which controllers a binding reads, the axis button's threshold and direction, the dead zone | `sdl_impl.cpp`: `SDLButtonFactory`, `SDLAxisButton`, `SDLAnalog`, `SDLAnalogFactory` |
| the axis over 32767, y turned over, the unit circle | `src/input_common/sdl/sdl_joystick.cpp`: `SDLJoystick::GetAxis`, `GetAnalog` |
| the keyboard stick | `src/input_common/analog_from_button.cpp` |
| parameter strings | `src/common/param_package.cpp` |
| the raw scale | `src/core/hle/service/hid/hid.cpp`: `MAX_CIRCLEPAD_POS`, `roundf` |

The test restates the bind table and compares it with `sdl_impl.cpp`'s text, and `hid.cpp`, `sdl_joystick.cpp` and `analog_from_button.cpp` for the constants, when `MH4N_AZAHAR_DIR` names a checkout of Azahar's sources at that commit (without it the test says so and checks the module against the restated table only: `MH4N_AZAHAR_DIR=~/azahar ctest --test-dir build/native-app --no-tests=error -R '^app_input_test$' --output-on-failure`).

## What N3-14 does with it

Creates an `InputMapper` over `MakeSdlGamepads()` (SDL's gamepad subsystem initialised), calls `Configure`, hands it every event the menu did not take, calls `Tick` once per loop and `SetMuted` as the menu shows and hides, sets `on_state` to `input_live::Find(rt)->Set` and `on_camera_turn` to the hooks' `AddCameraTurn`, and `SetCameraTurnActive` from the hooks' `CameraTurnActive`. Windows and macOS compile the same files (CI: `native-app.yml`).
