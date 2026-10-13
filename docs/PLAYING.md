# Playing the port

How to run Monster Hunter 4 Ultimate on a PC with this project: the game's own code, recompiled to native code on your machine, running on [Azahar](https://github.com/azahar-emu/azahar)'s 3DS core (system services, graphics, audio, multiplayer). Nothing here contains game data: everything game-related comes from **your own console's dump** and stays on your machine.

> **Status (October 2026):** the game runs on recompiled code from first boot (save creation, intro movie, title, character creation) into gameplay on the sand ship, at 60 fps, matching Azahar's own CPU emulation screen by screen. Tested in the cloud with Azahar's software renderer; the options marked *experimental* below haven't been tried in gameplay yet.

## What you need

- Your cartridge or digital copy of MH4U (USA) and a 3DS with [GodMode9](https://github.com/d0k3/GodMode9), to dump:
  - the **game** as a decrypted `.3ds` (or `.cci`/`.cxi`);
  - the **v1.3.0 update** as a `.cia` (GodMode9 can build one from the installed title). The port's code is made from this update.
- A PC: Windows 10/11 x64 or Linux x64 (macOS later).
- [Python](https://www.python.org/downloads/) 3.10 or newer (nothing else to install with it).
- This repository (`git clone`, or *Code → Download ZIP* on GitHub).
- The port's build of Azahar: on Windows, the `mh4u-port-windows` download of the latest successful run of this repository's [*Windows build* workflow](../../../actions/workflows/windows.yml) (GitHub account needed); on Linux, build it ([port/README.md](../port/README.md)).

## 1. Make the game's code library

From the repository folder:

```bash
# Windows (PowerShell or cmd); --fetch-compiler downloads the C compiler (llvm-mingw) the first time
py tools\build_game.py --dump "path\to\MH4U v1.3.0 update.cia" --install --fetch-compiler

# Linux (needs clang)
python3 tools/build_game.py --dump "path/to/MH4U v1.3.0 update.cia" --install
```

It checks that the update is v1.3.0, recompiles its code (a couple of minutes), compiles it (several minutes; `-j N` sets the number of parallel jobs) and installs `recomp/0004000000126300.dll` (`.so` on Linux) into Azahar's user folder (`%APPDATA%\Azahar` on Windows, `~/.local/share/azahar-emu` on Linux; give another folder after `--install` for a portable setup). Everything it writes under `build/` contains game code: keep it to yourself.

## 2. Set up Azahar

1. Start the port's Azahar.
2. *File → Install CIA…* → your v1.3.0 update `.cia` (the game runs the update's code, which is what was recompiled).
3. Add the folder with your game dump to the game list and start the game.

The log (*File → Open Folder → log folder*) says `running recompiled code from …` when the code library was found. If it says the running code `is not the code that was recompiled`, the update isn't installed (or is another version): the game still runs, but entirely in Azahar's interpreter, slowly.

Everything else is Azahar's own: controls, graphics settings, screen layouts, save data, cheats, texture packs. Two exceptions: **save states** aren't available while the game runs on recompiled code (the game's call stacks live on the PC's own stacks, which a save state can't capture), so save in the game; and cheats that rewrite code don't apply to recompiled functions (cheats that change data do).

## 3. Options

Write them in `mh4u.ini` in Azahar's user folder (next to the `recomp` folder), one `key = value` per line:

```ini
# frame-rate cap: the game runs at 30 on a 3DS; its engine measures time, so gameplay speed stays right
fps = 60
# above 60 fps: emulated screen refresh rate in Hz (experimental; set fps to the same value)
refresh_rate = 0
# widen the 3D view to this aspect ratio (experimental, see below)
widescreen = 16:9
# render the wider image itself instead of stretching a 400x240 one (16:9 tried in gameplay)
wide_render = 1
# write the engine's frame timing to the log every n frames
trace_frames = 0
# write touch-panel activity to the log (to map panels to buttons)
trace_panels = 0
# experimental: bits of the game's render settings word to clear/set every frame, e.g.
# 0x300000 = no dithering, 0x400 = the game's own 2D anti-aliasing (docs/research/system-services.md)
render_flags_clear = 0
render_flags_set = 0
```

Environment variables override the file: `MH4U_FPS`, `MH4U_REFRESH_RATE`, `MH4U_WIDESCREEN`, `MH4U_WIDE_RENDER`, `MH4U_TRACE_FRAMES`, `MH4U_TRACE_PANELS`.

- **60 fps** *(experimental)*: the engine's frame limiter is raised to the cap. A frame must also fit in one screen refresh of emulated time: if the game stays below 60, raise *CPU Clock Speed* (*Emulation → Configure → Debug*). All players in a multiplayer session should use the same value. **Above 60** *(experimental)*: the game presents at most one frame per screen refresh, so also set `refresh_rate` to the same value (e.g. 120 or 144) and give the emulated CPU more clock; video cutscenes that count screen refreshes might then play too fast.
- **Widescreen** *(experimental)*: the 3D view is widened to the given aspect ratio, and *View → Screen Layout → Single Screen* then shows the top screen at that shape (16:9, 16:10, 21:9 and 4:3 exactly; other values stretch it to the window). In *Custom Layout* you can size the top screen yourself and lay the bottom screen over it with *Bottom Screen Opacity*. With `wide_render = 0` this stretches a 400x240 image, HUD included. With `wide_render = 1` the game draws a wider image instead (424x240 for 16:9: render targets, viewports and the copy to the screen are made wider): the 3D view fills the width and the HUD keeps its proportions, centred in the middle 400 pixels until its edge elements get anchored to the screen edges ([research/widescreen.md](research/widescreen.md)). It goes up to 512x240 (about 2.1:1); beyond that, as for 21:9, the rest is stretched.
- **The bottom screen**: use Azahar's screen layouts (side by side, large top screen with a small bottom one, separate windows, or a translucent overlay in *Custom Layout*) and the mouse as the stylus. Button bindings and a single-screen HUD for the touch panels are planned ([research/second-screen.md](research/second-screen.md)).

## Your 3DS save

To continue your 3DS progress: back the save up on the console with [Checkpoint](https://github.com/BernardoGiordano/Checkpoint) (it saves the files inside the save, not a raw image), then right-click the game in Azahar's list → *Open → Save Data Location* and copy those files there while the game isn't running. Keep a copy of the backup.

## 4. Multiplayer

MH4U's local multiplayer (Gathering Hall) works over the internet through Azahar's network rooms: *Multiplayer → Browse Public Rooms / Create Room / Direct Connect to Room*. Everyone needs the **same build** of the port. Nintendo Network online play is gone; DLC quests can come from your own console's extra data.

## 5. Graphics

Azahar's renderer settings apply unchanged (*Emulation → Configure → Graphics*): internal resolution (up to many times the 3DS's), texture filtering, post-processing shaders and custom texture packs (PNG replacements in `load/textures/0004000000126300/` of Azahar's user folder).

## Troubleshooting

| Symptom | Cause |
|---|---|
| `no recompiled code for 0004000000126300` in the log | the library isn't in `<Azahar user folder>/recomp/`, or has another name |
| `... was built against a different runtime` | the library was made with an older version of this repository: run `build_game.py` again |
| `no hook point for ...` in the log | same: rebuild the library so the port's options work |
| `this code.bin isn't a version the project knows` | the dump isn't the USA v1.3.0 update |
