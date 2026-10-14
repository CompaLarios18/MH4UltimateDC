# Playing the port

How to run Monster Hunter 4 Ultimate on a PC with this project: the game's own code, recompiled to native code on your machine, running on [Azahar](https://github.com/azahar-emu/azahar)'s 3DS core (system services, graphics, audio, multiplayer). Nothing here contains game data: everything game-related comes from **your own console's dump** and stays on your machine.

> **Status (October 2026):** the game runs on recompiled code from first boot (save creation, intro movie, title, character creation) through the village, quests, expeditions and hunts, with sound, matching Azahar's own CPU emulation screen by screen where compared. Played on Windows (Azahar's Qt interface, Vulkan) at 60 and 120 fps with 16:9 `wide_render`, the kick and target camera on buttons and the touch-screen overlay; tested in the cloud with Azahar's software renderer, including two instances meeting in a Gathering Hall over a network room and going on a quest together (also with one of them on Azahar's own CPU emulation, as a friend on official Azahar would be). Options still marked *experimental* below have been tried less.

## What you need

- Your cartridge or digital copy of MH4U (USA) and a 3DS with [GodMode9](https://github.com/d0k3/GodMode9), to dump:
  - the **game** as a decrypted `.3ds` (or `.cci`/`.cxi`);
  - the **v1.3.0 update** as a `.cia` (GodMode9 can build one from the installed title). The port's code is made from this update.
- A PC: Windows 10/11 x64, Linux x64, or a Mac with Apple silicon (the macOS app is not tried yet). On Windows 11, **Smart App Control** must be off: it blocks the unsigned programs and libraries involved (Azahar's DLLs, the compiler, the code library) with "Bad Image ... 0xc0e90002". *Windows Security → App & browser control → Smart App Control settings → Off*; since the April 2026 update (KB5083769) it can be turned back on later without reinstalling Windows.
- [Python](https://www.python.org/downloads/) 3.10 or newer (nothing else to install with it).
- This repository (`git clone`, or *Code → Download ZIP* on GitHub).
- The port's build of Azahar: on Windows, the `mh4u-port-windows` download of the latest successful run of this repository's [*Windows build* workflow](../../../actions/workflows/windows.yml); on Linux, the `mh4u-port-linux` download (an AppImage; tried on Ubuntu 24.04: it runs the game on recompiled code) of the [*Linux build* workflow](../../../actions/workflows/linux.yml), on a Mac with Apple silicon, the `mh4u-port-macos` download of the [*macOS build* workflow](../../../actions/workflows/macos.yml) (an app, not tried on a Mac yet; right-click it and choose *Open* the first time); or build it yourself ([port/README.md](../port/README.md)). Downloads from workflow runs need a GitHub account.

## 1. Make the game's code library

On Windows the port's package has it ready: install [Python](https://www.python.org/downloads/) 3.10 or newer (tick *Add python.exe to PATH*), close Azahar, double-click `Build code library.bat` in the package and choose your v1.3.0 update `.cia`. It runs the same command as below with the tools in the package's `mh4u-tools` folder, then installs the update in Azahar (step 2 below is then done), and the first time it also writes a `mh4u.ini` with 60 fps, 16:9 and the touch screen in a corner, and picks Azahar's *Custom Layout*.

Otherwise, from the repository folder:

```bash
# Windows (PowerShell or cmd); --fetch-compiler downloads the C compiler (llvm-mingw) the first time
py tools\build_game.py --dump "path\to\MH4U v1.3.0 update.cia" --install --fetch-compiler

# Linux: uses clang if it's installed; otherwise --fetch-compiler downloads Zig's C compiler
# (about 80 MB, from PyPI), e.g. on a Steam Deck; it builds the same library about a third
# slower than clang (tried on the full game: it runs into gameplay like the clang build)
python3 tools/build_game.py --dump "path/to/MH4U v1.3.0 update.cia" --install --fetch-compiler

# macOS: the Xcode command-line tools' clang (xcode-select --install) and Python 3.10 or
# later (python.org or Homebrew; the command-line tools' own Python is older)
python3 tools/build_game.py --dump "path/to/MH4U v1.3.0 update.cia" --install
```

It checks that the update is v1.3.0, recompiles its code (a couple of minutes), compiles it (several minutes; `-j N` sets the number of parallel jobs) and installs `recomp/0004000000126300.dll` (`.so` on Linux, `.dylib` on macOS) into Azahar's user folder (`%APPDATA%\Azahar` on Windows, `~/.local/share/azahar-emu` on Linux, `~/Library/Application Support/Azahar` on macOS; give another folder after `--install` for a portable setup). Everything it writes under `build/` contains game code: keep it to yourself.

## 2. Set up Azahar

1. Start the port's Azahar.
2. *File → Install CIA…* → your v1.3.0 update `.cia` (the game runs the update's code, which is what was recompiled).
3. Add the folder with your game dump to the game list and start the game.

The log (*File → Open Folder → log folder*) says `running recompiled code from …` when the code library was found. When the game starts without it, the port's Azahar says why in a message (no library, an old one, or the game's code isn't the one it was built from: the update isn't installed, or a code mod changed) and runs the game on Azahar's own CPU emulation, without the port's options.

Everything else is Azahar's own: controls, graphics settings, screen layouts, save data, cheats, texture packs. Two exceptions: **save states** aren't available while the game runs on recompiled code (the game's call stacks live on the PC's own stacks, which a save state can't capture), so save in the game; and cheats that rewrite code don't apply to recompiled functions (cheats that change data do).

## 3. Options

Write them in `mh4u.ini` in Azahar's user folder (next to the `recomp` folder), one `key = value` per line. *Tools → MH4U Port Options...* in the port's Azahar opens that file in a text editor, and first makes it with the recommended settings when there is none. Changes apply the next time the game starts. All the options:

```ini
# frame-rate cap: the game runs at 30 on a 3DS; its engine measures time, so gameplay speed stays right
fps = 60
# emulated screen refresh rate in Hz: 0 = the 3DS's, or the fps when that is above 60 (the game
# shows at most one frame per refresh, so more than 60 fps needs more refreshes)
refresh_rate = 0
# widen the 3D view to this aspect ratio (experimental, see below)
widescreen = 16:9
# render the wider image itself instead of stretching a 400x240 one (16:9 tried in gameplay)
wide_render = 1
# with wide_render, move the hunting HUD's sides (health, stamina, sharpness, party list on
# the left; the item selection on the right) out to the screen's edges (0 = keep it centred)
hud_edges = 1
# write the HUD's layout objects to the log every n frames (to find more side objects)
trace_layout = 0
# write the engine's frame timing to the log every n frames
trace_frames = 0
# a button for the kick, which on a 3DS only the touch screen's kick panel does
# (A B X Y L R ZL ZR START UP DOWN LEFT RIGHT; ZR is the right trigger on a controller)
kick = ZR
# a button for the target camera panel (the large-monster icon on the touch screen): one
# press toggles the camera's target on the monster, or with two large monsters goes to the
# first, the second, then off; L then turns the camera to it, as with the panel
target = ZL
# View > Screen Layout > Custom Layout draws the touch screen over the top screen, which
# fills the window: corner, height in % of the window's, opacity in % (off = Azahar's own)
touch_overlay = top-right 30 85
# in a quest, that corner shows the map alone, where your panel layout has it (map = at the
# same scale, map 40 = 40% of the window's height), and the whole touch screen again when
# something else covers the panels (menus, Hunter Search); off = always the whole touch screen
quest_overlay = map
# write touch-panel activity to the log (to map panels to buttons; doesn't see the taps yet)
trace_panels = 0
# the game's dither pattern (on by default, as on a 3DS): an upscaled picture shows it as a fine
# grid over everything, so turn it off when Internal Resolution is above 1x
dither = off
# multiplayer: keep the port's consoles in a network room together, so that one whose PC stalls
# for a moment isn't dropped by the others (on by default; off, or all = with every port
# console in the room, not only the ones you play with); net_sync_slack: ms of lead before a
# console waits (200)
net_sync = on
# experimental: bits of the game's render settings word to clear/set every frame, e.g.
# 0x400 = the game's own 2D anti-aliasing (docs/research/system-services.md)
render_flags_clear = 0
render_flags_set = 0
```

Environment variables override the file: `MH4U_FPS`, `MH4U_REFRESH_RATE`, `MH4U_WIDESCREEN`, `MH4U_WIDE_RENDER`, `MH4U_KICK`, `MH4U_TARGET`, `MH4U_TOUCH_OVERLAY`, `MH4U_QUEST_OVERLAY`, `MH4U_HUD_EDGES`, `MH4U_DITHER`, `MH4U_RENDER_FLAGS_SET`, `MH4U_RENDER_FLAGS_CLEAR`, `MH4U_NET_SYNC`, `MH4U_NET_SYNC_SLACK`, `MH4U_TRACE_FRAMES`, `MH4U_TRACE_PANELS`, `MH4U_TRACE_LAYOUT`.

- **60 fps**: the engine's frame limiter is raised to the cap; in the opening scenes the game holds 60 with each frame advancing 1/60 s, so it runs at normal speed. A frame must also fit in one screen refresh of emulated time: if a busy scene stays below 60, raise *CPU Clock Speed* (*Emulation → Configure → Debug*). All players in a multiplayer session should use the same value. **Above 60** *(experimental)*: set `fps` to your monitor's rate, e.g. 120 (the emulated screen's `refresh_rate` follows it unless you set it). Tried at 120: menus, loading, the real-time cutscene and walking on the sand ship run at 120 fps at normal speed (a loading race that more frames per second bring out at the start of quests is worked around). Movies (the opening film) still play at their own 30 fps. Each frame then has 1/120 s of emulated time, so busy scenes need a higher *CPU Clock Speed*. Played on Windows at 120 fps (Ryzen 5 5600X, Vulkan, 180 Hz monitor), hunts included: no problems reported.
- **Widescreen** *(experimental)*: the 3D view is widened to the given aspect ratio and every screen layout (*View → Screen Layout*) shows the top screen at that shape: stacked above the bottom screen, side by side, *Large Screen* with the touch screen small in a corner, or *Single Screen* for the top screen alone (16:9, 16:10, 21:9 and 4:3 exactly; other values stretch it to the window). In *Custom Layout* you can size the top screen yourself and lay the bottom screen over it with *Bottom Screen Opacity*. With `wide_render = 0` this stretches a 400x240 image, HUD included. With `wide_render = 1` the game draws a wider image instead (424x240 for 16:9: render targets, viewports and the copy to the screen are made wider): the 3D view fills the width and the HUD keeps its proportions. The hunting HUD's sides go to the screen's edges (`hud_edges`): health, stamina, sharpness and the party list on the left, the item selection on the right (tried on an expedition and in a two-player quest); the rest stays in the middle 400 pixels ([research/widescreen.md](research/widescreen.md)). Ultrawide screens get it too: 21:9 is drawn 560x240 and 32:9 856x240 (past 512 pixels the game's main render target goes from 512 to 1024 rows, the most the 3DS's GPU draws; tried in Val Habar and on an expedition: the 3D view fills the width, the HUD stays centred with its sides at the edges, and the touch screen is unchanged). A shape narrower than the 3DS's own 5:3 (16:10 on a Steam Deck, 4:3) narrows the 3D view instead, so that the picture squeezed to that shape keeps its proportions (the HUD is squeezed with it); leave `widescreen` off to keep the game's own shape with thin bars.
- **The bottom screen**: use Azahar's screen layouts (side by side, large top screen with a small bottom one, separate windows) and the mouse as the stylus. **`touch_overlay`** makes *Custom Layout* a single-screen layout that follows the window's size: the top screen as large as the window allows (full screen on a 16:9 monitor with `widescreen = 16:9`), the touch screen drawn over it in a corner, translucent if you like, and clickable there. Choose the corner that hides the least: the item bar is at the bottom right of the top screen. With `touch_overlay = off` *Custom Layout* is Azahar's own (fixed positions from *Configure → Layout*). **In a quest the corner shows only the map** (`quest_overlay`): the map panel at the same scale as the whole touch screen, wherever your panel layout puts it (*Customize Panels* in the quest menu), and a click on it is a tap on the map, as on the 3DS (it switches the map's zoom). The camera pad, the target camera, the item shortcuts and the *Hunter Search*/*Chat* bar are hidden then: the camera turns with the C-stick (a controller's right stick), and `target` and `kick` put those panels on buttons. Whenever something else is on the touch screen (the quest menu, Hunter Search) the whole touch screen comes back. `quest_overlay = map 40` makes the map 40% of the window's height; `quest_overlay = off` keeps the whole touch screen in quests (tried in the cloud on an expedition: the map in, the Hunter Search screen and the quest menu out). To show the touch screen only when you need it (the map, say): in *Emulation → Configure → Layout → Customize layout cycling* tick only *Single Screen* and *Custom*; then *Toggle Screen Layout* (F10 by default, *Configure → Keyboard Hotkeys*; on a controller, bind it in *Configure → Controller Hotkeys* to a button or a two-button chord) switches between the game alone and the game with the touch screen in its corner. **`kick`** puts the kick on a button: the game gets the same action as from the kick panel (tried on the sand ship's deck). The button keeps its own action too; ZR's isn't known to do anything in MH4U without a Circle Pad Pro. **`target`** does what a tap on the target camera panel does (the panel has to be in your panel layout, as it is by default); a monster you haven't seen yet in the quest gets the game's "no" sound, as with the panel. Played on Windows with `target = ZL`: it works as the panel does. More on the touch screen in [research/second-screen.md](research/second-screen.md).

## Your 3DS save

To continue your 3DS progress: back the save up on the console with [Checkpoint](https://github.com/BernardoGiordano/Checkpoint) (it saves the files inside the save, not a raw image), then right-click the game in Azahar's list → *Open → Save Data Location* and copy those files there while the game isn't running. Keep a copy of the backup.

## 4. Multiplayer

MH4U's local multiplayer (Gathering Hall) works over the internet through Azahar's network rooms. Azahar has no public room list of its own (*Browse Public Rooms* stays empty unless you give it a third-party lobby server), so one player hosts a room and the others connect to it directly:

1. **Everyone**: give yourself a name in *Emulation → Configure → System → Username*. It is your name in the room, and a room lets in only one player per name (Azahar's default is AZAHAR for everybody).
2. **The host**: *Multiplayer → Create Room*, type *Unlisted* (the port's Azahar picks it; *Public* needs a lobby server, and without one official Azahar fails with "Failed to announce the room to the public lobby"), port 24872 unless you change it, a password if you like. The others must reach that port on the host's PC: forward UDP port 24872 on the host's router to it, or, without access to the router (or behind a provider's shared address), put all the PCs in one virtual network (Tailscale, ZeroTier or Radmin VPN) and use the host's address in it.
3. **The others**: *Multiplayer → Direct Connect to Room*, the host's address (its public IP, or its address in the virtual network) and port.
4. In the game: *Local Play* at the Gathering Hall.

One player creates a hall, the others find and join it; one posts a quest at the Quest Counter, the others join it at the Quest Board and stand by at the Quest Gate, and the one who posted it departs (tried with two instances in the cloud). **The others don't need the port**: official Azahar (2125 or later; rooms speak the same protocol) with their own copy of the game works too, as long as everyone has the v1.3.0 update (tried in the cloud: one instance on recompiled code hosting, one on Azahar's own CPU emulation joining; they met in the hall and went on a quest together). Three and four players were tried too (all on the port): four joined the hall at once, posted and joined one quest and loaded into it together. **Players don't need the same frame rate**: in the cloud, one player went to 120 fps (`fps`/`refresh_rate`) as an Arena quest loaded while the other stayed at the game's 30, and they stayed together in the arena to the end of the run (about a minute and a half), each seeing the other move and swing. **Join a hall one at a time**: official Azahar gives two consoles that join in the same instant the same id, and the game drops one of them ("The connection was lost"); the port's Azahar fixes that, but the fix needs both ends. Nintendo Network online play is gone; DLC quests can come from your own console's extra data.

The game drops a player whose game stops for about a second, and on a PC a stutter does that (shader compilation, a slow disk). **The port keeps its consoles together** (`net_sync`): each one tells the others in the room its emulated clock, and one that runs more than 200 ms ahead of a player it plays with waits for that player. So when one PC stalls, the others pause with it for a moment instead of dropping it, and a PC that can't keep full speed sets everyone's pace (tried in the cloud: with it, a 3-second freeze of the host and a stretch of second-long stalls on both sides left both players in the hall, and two players went through a quest's start with 160 ms of network round trip between them; without it, the freeze dropped the host). It works between port players only (official Azahar doesn't send its clock), so with official Azahar in the room still avoid stutters: keep *Use disk shader cache* on, and with Vulkan turn on *Enable async shader compilation* (*Emulation → Configure → Graphics*).

## 5. Graphics

Azahar's renderer settings apply unchanged (*Emulation → Configure → Graphics*): internal resolution (up to many times the 3DS's), texture filtering, post-processing shaders and custom texture packs (PNG replacements in `load/textures/0004000000126300/` of Azahar's user folder).

With a higher internal resolution, set `dither = off`. The game draws a dither pattern over both screens, which hides banding on the 3DS's screens. Azahar scales that pattern up with the rest of the picture, so it shows as a fine grid over everything. Without it, the picture is clean (compared in the cloud; the community's "Trinity Disabler" cheat turns the same bits off).

## 6. Mods

Mods go where Azahar looks for them (right-click the game → *Open Mods Location*, i.e. `load/mods/0004000000126300/` in Azahar's user folder):

- **Files** (textures, models, text, quests): in `romfs/`, as for Azahar. The port doesn't change how the game reads its files.
- **Code mods** (`exefs/code.ips`, `exefs/code.bps` or a whole `exefs/code.bin`, made for the USA v1.3.0 update): Azahar patches the game's code with them when it starts. One that only changes the game's data works as it is. For example, the crafting patch of [anco's MH4U quality-of-life project](https://codeberg.org/anco/mh4u_qol) (every combination succeeds) changes 136 bytes of data in v1.3.0; tried in the cloud. One that changes the game's code needs the code library built again with the mod in place (step 1 again: `build_game.py --install` and `generar_codigo.ps1` apply the mods they find in that folder and say whether a mod changes code). Otherwise the port sees code it didn't recompile and runs everything in Azahar's interpreter, which is far too slow to play. Build again whenever you add, change or remove such a mod.
- **Cheats** that write data work (*Cheats* in the game's right-click menu). Ones that rewrite code don't take effect in recompiled functions.

## Troubleshooting

| Symptom | Cause |
|---|---|
| At start: *The MH4U PC port's code library isn't installed* (in the log: `no recompiled code for 0004000000126300`) | the library isn't in `<Azahar user folder>/recomp/`, or has another name: step 1. Until then the game runs on Azahar's own CPU emulation, without the port's options |
| At start: *... couldn't be used* (in the log: `... was built against a different runtime`) | the library was made with an older version of this repository: build it again |
| At start: *The game's code isn't the code the MH4U PC port's library was built from* | the v1.3.0 update isn't installed in Azahar (*File → Install CIA...*), or a code mod was added, changed or removed since the library was built (build it again). The game runs on Azahar's own CPU emulation meanwhile |
| At start: *... is older than this Azahar: N of the port's hook points are missing* (in the log: `no hook point for ...`) | the library was built before this Azahar's version: build it again so all of the port's options work |
| `this code.bin isn't a version the project knows` | the dump isn't the USA v1.3.0 update |
| Windows: `azahar.exe - Bad Image ... Error status 0xc0e90002` | Smart App Control blocks unsigned files: turn it off (see *What you need*) |
| After a loading screen the hunter falls through a dark void, then *Quest Failed* | a loading race in the game, which more than 60 fps brings out: the quest's file is left out of the list it is copied from ([research](research/system-services.md#above-60-fps-what-the-game-does-at-120-hz-runtime-october-2026)). The port works around it since October 2026 (a code library and Azahar build from before that don't). If it still happens, turn on *Emulation → Configure → Debug → Force deterministic async operations* and report it |
