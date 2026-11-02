**Monster Hunter 4 Ultimate on PC**, and now on Android, through Azahar's 3DS core with the game's own code recompiled for your CPU, plus PC options: higher frame rates, widescreen up to 32:9, the touch screen over the game, and multiplayer over the internet.

🇲🇽 *Port de MH4U para PC, y ahora también para Android: 60/120 fps, pantalla ancha, la pantalla táctil encima del juego y multijugador por internet. Necesitas tu propia copia del juego y su actualización: la v1.3.0 si es de EE. UU., la v1.1.0 si es europea.*

## What's new in 0.1.6

- **The menu, sorted by task.** Esc's menu has eight tabs now: *Display* (frame rate, vsync, the frame-rate counter, CPU clock speed, full screen, the renderer, widescreen), *Graphics* (internal resolution, smooth scaling, anti-aliasing, dither, texture packs, the game's effects), *Touch screen* (where the screens go, the touch screen in its corner, the map and the target camera in quests, *Edit HUD layout*), *Controls* (*Play with*, the key guides, the mouse, the controller), *Keys* (every key, in groups), *Gameplay* (the kick and the target camera on buttons, the end of quests, fast forward and pause, touch taps), *Audio* and *Online* (*Multiplayer* before, with net sync at its top). What was in *Game* went to *Display*, *Graphics*, *Touch screen*, *Gameplay* and *Online* (net sync), and *Quit game* is a button at the top. *Texture packs* and *Launcher before the game* can be changed there too.
- **A search at the top**: Ctrl+F (Cmd+F on a Mac) or Y on a controller, then a setting's name or a word like fps, map, kick or bloom; Enter goes to it, and it flashes where it is. With a controller you type on keys drawn on the screen, then Y (or Start) takes you to the results and A goes to one.
- **A reason on every setting that can't apply.** A setting that can't take effect now stays in its place, greyed out, with a line that says why and a link to what to change (*Only with Draw the wider picture on*, *Not with the software renderer*...). Those that apply from the next start say so, and a renderer that didn't start is named (*In use: OpenGL (Vulkan didn't start: see mh4u_log.txt)*).
- **With a controller**: LB/RB change tabs, LT/RT jump between a tab's sections, left and right change most lists and sliders without opening them, and B goes back one step at a time (the keys on the screen, the search, then the menu). Holding A on a slider no longer asks for a value to type, in the launcher too. *Edit HUD layout* works with a controller (LB/RB choose the map or the target camera, the D-pad moves it, LT/RT resize it) and with the keyboard, and a Steam Deck's touchscreen works in the whole menu.
- Changed on purpose: *Reset keys to the defaults* asks first, and leaves *Fast forward mode* and the touch taps' keys as they are; *Hold for* shows only while *End the quest* isn't *None*.
- **Touch taps**: keys that touch the touch screen at a point while you hold them, for an item shortcut, the target camera or any other panel, without reaching for the mouse. In *Esc → Gameplay → Touch taps*: *Add a tap*, choose its key and put its point on the picture of the touch screen (or touch the panel in the game, then *Use the last touch*). Up to 8, each with a name for the key guides. A controller's spare button can be one: map it to the tap's key in Steam Input.
- **When the quest map can't show, the app says why.** Without the map panel in your panel layout (*Customize Panels* in the quest menu) the corner keeps the whole touch screen in quests; the menu now says so under the map's settings and in *Edit HUD layout*, as for the other reasons (the screens, *Touch screen over the game* or *Only the map in quests* off, an older code library), and F12 (or M, or the right stick's click) says it at the top of the screen instead of doing nothing.
- **A game file that won't play is caught before *Play***: one decrypted wrongly, damaged or cut short is told from its headers, and Setup says what to do. If the game still stops as it starts with an error of its own, the message says what to try (mods off, a new dump, the update again) and where the log is; before, it said *The game stopped:* and nothing else. When a code mod (`code.ips` or `code.bps`) doesn't fit the game's code, the game won't start, and the message now says so and how to turn the mod off.
- **A v1.3.0 update whose `.cia` says v0.0.1 or v1.0.0** (some tools that decrypt or rebuild a `.cia` reset that label) is told by its code: Setup installs it, and says so.
- **The launcher's own file browser**: *Browse in the launcher* is next to each file choice (the game, the update, mods, texture packs), with folders and the files that will do, places on the left (home, Downloads, the last folder, the drives, a Steam Deck's SD card) and a line to type a path into; the D-pad and A, the mouse or the keyboard. It opens by itself with a controller in hand, in Steam's Game Mode, on a Linux system without zenity or kdialog (before, you typed the path), and when the system's file dialog fails (the Steam Deck's *Service crash*).
- **Linux: the launcher's build works where the terminal's did** (Fedora): the tools it runs no longer get the AppImage's own libraries and variables. On every system, a build that fails says how it ended, and *Copy the output* copies everything it printed, with the app's version, for an issue (when a file didn't compile, leave out the lines of recompiled code the compiler quotes: that code is made from the game's); files whose compiler the system stopped (out of memory, say) are compiled again one at a time.
- The code library stays as it is: this release doesn't need it built again.
- **On Android nothing changes**: what's new above is in the PC packages (the app and its tools), so 0.1.5's APK can stay. This release's APK won't install over 0.1.5's, which was signed with a key of its own build: to install it anyway, uninstall MH4U first (your games, saves and code library are in the user folder you chose, which stays).

## What's new in 0.1.5

- **Android**: `mh4u-port-android.apk` is Azahar's Android app with the port inside, for arm64 phones and handhelds with Android 10 or newer (thanks to james2doyle, who brought the port to Android). It's named **MH4U** and installs next to Azahar without replacing it. As on a PC, the game's code library is built on your PC from your own v1.3.0 update, then copied to the device: the steps are under *How to play (Android)* below. New and experimental: reports are welcome in an issue.
- **On PC nothing changes**: the Windows, Linux and macOS packages carry 0.1.4's app; their tools can now build the Android library too (`build_game.py --android`).
- This APK is signed with a key of its own build. If a later release's APK won't install over it, uninstall this one first: your games, saves and code library are in the user folder you chose, which stays.

## What's new in 0.1.4

- **Pause and fast forward**, playing alone: **P** stops the game where it is (the picture and the menu stay), **Tab** runs it at 2x for the walks and the gathering. The speed (1.5x to 4x, or as fast as your PC can) and whether Tab is held or pressed once are in Esc → Gameplay → Speed, with the keys. In a room they're off (the others' games run on with yours), and the keys say so. Save states still aren't possible with the recompiled code: save in the game.
- **Rooms on your own network, with no address.** On one Wi-Fi or router (laptops, PC handhelds, a laptop's hotspot), the host hosts a room and the others find it under **On this network** (Esc → Online → Join a room, or the launcher's Online screen) and press *Join*: nothing to copy, nothing to forward. If a room doesn't show up, allow the app in the host's firewall (it answers on UDP port 24873). `lan_discovery = off` in `mh4u-app.ini` keeps a hosted room out of the search.
- **Cheats no longer slow the game to a crawl**: with code cheats enabled on a New 3DS, the game could fall back to the slow interpreter (0-8 fps in the intro). The code is now checked once, at boot. Code cheats don't apply to recompiled code; data cheats work.
- **Android, from source** (thanks to james2doyle): the port's recompiled code and hooks now run inside Azahar's Android app on arm64 devices, built from source. There's no Android package in this release yet.

## What's new in 0.1.3

- **Keyboard and mouse like Monster Hunter Rise on PC.** The mouse turns the camera itself, like a PC game's mouse look (not by pushing a stick: no speed cap, no acceleration), with sensitivity, up/down sensitivity and invert in Esc → Controls. Left click attacks (X), right click or F is A (attack, gather, carve, talk), Space evades, E uses an item or sheathes, Shift dashes, the side button guards, Ctrl or the wheel's click resets the camera, Q targets a monster, C kicks / skips, the **wheel scrolls the item bar**. Every button takes two keys or mouse buttons. Hold **Alt** to use the mouse on the touch screen. **Rebuild the code library once** (the launcher asks): the mouse camera is a new hook.
- **Key guides**: while you play with the keyboard and mouse, the keys for everything the hunter does show small in a corner, each with the 3DS button the game's own prompts name. F1 hides them; corner and on/off in Controls.
- **Auto, controller or keyboard and mouse**: the app switches by what you used last (with a controller, the mouse is the stylus as before). Keys you changed yourself stay; untouched old keys move to the new layout.
- **Updates from the launcher**: it looks for a new release when it opens; *Update now* downloads it, checks it and restarts. Starting with this version (from 0.1.2, download this one by hand once).
- **A close button** on the launcher (it opens full screen, without a title bar).
- **Steam Deck**: B no longer opens the port's menu when Steam Input sends Esc along with it.

## What's new in 0.1.2

- **End the quest together.** Hold **ZL+ZR** (Q+C) for a second once a quest is complete to end the wait before the return, as if the carving time had run out. Online, the quest ends once every player with the port has held it, and the screen names who hasn't. `skip_end` / `skip_end_hold` in `mh4u.ini` and the menu.
- **The scene skip is just that again, weapon drawn or not.** ZR / C taps the touch screen's own *Skip* (the scene of the monster's end) and leaves the carving time alone. 0.1.1 ended the quest instead when the tap didn't take.
- **An icon of its own** for `mh4u.exe`, the Mac app, the AppImage and the windows.
- **CPU clock speed** in the menu's Display tab (`cpu_clock` in `mh4u-app.ini`), Azahar's, for high frame rates in busy scenes; and the game's **anti-aliasing** switchable (`antialiasing`), to save its pass at a high internal resolution.

## What's new in 0.1.1

- **Skip with the weapon drawn.** The *Skip* button after a quest (ZR / C) did nothing while the weapon was out, because its tap is the kick's action; the port ended the wait itself when the tap didn't take (changed in 0.1.2). Rebuild the code library once (the launcher asks).
- **Sharper picture.** The game drew the top screen twice (one pass per eye) even with Azahar's 3D at 0, which showed as a doubled, soft image; `stereo_3d = off` (the default, also in Esc → Graphics → Image quality) draws it once and saves a scene pass.
- **Faster rebuilds.** Unpacking a new package no longer recompiles the whole code library: only the files that changed.

## No game data

These packages contain **no game code or data**. You need your own decrypted copy of Monster Hunter 4 Ultimate and its update (`.cia`): the USA game with its **v1.3.0 update**, or the European one with its **v1.1.0 update**. The game's code library is built on your computer from your update, in one step.

## Downloads

| System | File | Status |
|---|---|---|
| Windows 10/11 (x64) | `mh4u-port-windows.zip` | played for hours, hunts included, at 120 fps in earlier versions; 0.1.6's app not tried with the game yet |
| Linux (x86-64, Steam Deck too) | `mh4u-port-linux.tar.gz` | two AppImages, the app and Azahar; Azahar tried on Ubuntu 24.04 in an earlier version |
| macOS (Apple silicon) | `mh4u-port-macos.zip` | two apps, MH4U and Azahar; builds, not tried on a Mac yet |
| Android 10+ (arm64) | `mh4u-port-android.apk` | Azahar's Android app with the port, named MH4U; new and experimental |

Each PC package has the port's own app (`mh4u.exe`, `mh4u.AppImage`, `MH4U.app`), which opens the game directly, and Azahar with the port (`azahar.exe`, `azahar-mh4u.AppImage`, `Azahar-mh4u.app`) for everything the app doesn't do yet.

## How to play (Windows)

1. Install [Python](https://www.python.org/downloads/) 3.10 or newer (tick *Add python.exe to PATH*).
2. Unzip and double-click `mh4u.exe`. The first time its launcher opens on *Setup*: choose your game and its update `.cia` (v1.3.0 for the USA game, v1.1.0 for the European one); it installs the update and builds the game's code library (the first time it downloads the C compiler, about 180 MB). (`Build code library.bat` still does the same from a console.)
3. *PLAY*. The game opens full screen (F11: a window); **Esc** (or a controller's Guide, or Back+Start) opens the menu over the game.

The launcher also has *Mods* (several at once, in an order, conflicts shown), *Online* (a lobby's rooms, joining, hosting) and *Settings*. The in-game menu changes the game's options (frame rate, widescreen, the touch screen, filters…), the display, sound and controls, mostly at once, and saves them in `mh4u.ini` and `mh4u-app.ini` (in `%APPDATA%\Azahar`). `azahar.exe` plays the game too (*File → Load File…*), installs other CIAs and has Azahar's own settings. If Windows 11's Smart App Control blocks `mh4u.exe` or `azahar.exe` (unsigned), it has to be turned off. Linux and macOS: the steps are in `README-mh4u.txt` in each package.

## How to play (Android)

1. On your PC, build the game's code library for Android from your update (v1.3.0; Europe: v1.1.0), with the tools in one of this release's PC packages (the APK and the library go together: one built by another release's tools may be refused) and Python 3.10 or newer. Windows, in the unzipped `mh4u-port-windows` folder: `py mh4u-tools\tools\build_game.py --dump "path\to\update.cia" --android --fetch-compiler`; Linux or macOS: `python3 mh4u-tools/tools/build_game.py --dump "path/to/update.cia" --android --fetch-compiler`. It writes `mh4u-tools/build/game/recomp/android/0004000000126300.so` (`0004000000126100.so` from Europe's update; no Android NDK needed).
2. Install `mh4u-port-android.apk` on the device (Android asks to allow installs from your browser or file manager) and open it; choose its user folder when it asks, as Azahar does. If an earlier release's APK is installed and this one won't install over it, uninstall that one first: your games, saves and code library are in the user folder you chose, which stays.
3. Copy the `.so` into that folder's `recomp` folder (create it), by USB or any file sync.
4. In the app, install your update `.cia` and add your game, as in Azahar. The log says `running recompiled code from …` once the library is found; the port's options are in `mh4u.ini` in the user folder (the first one fills the screen: `widescreen = match`). Details: [docs/PLAYING.md, Android](@REPO@/blob/@COMMIT@/docs/PLAYING.md#7-android-experimental).

## What's in it

- **The port's own app**: a launcher (setup, mods, online rooms, settings), then the game in a window of its own, full screen, with the keyboard and mouse (Rise-style, the mouse on the camera, key guides on screen) or a controller mapped on its own (like MH3U on a Wii U); updates from the launcher; Vulkan, OpenGL or software rendering; a menu over the game (Esc) for options and controls; the 3DS keyboard (names, chat) drawn by the app; a frame-rate counter (F3, off by default).
- **Rooms while you play**: join, host or leave a room from the menu's *Online* tab without restarting the game; the host copies an invite (`mh4u://…`), friends paste it and join; a lobby's rooms are listed there too.
- **Mod catalog**: the launcher's *Mods → Catalog* installs the HD texture pack and four other community mods with one click (from their authors' own downloads; the three code mods only for the USA game).
- **Servers of our own**: a lobby server that brings back public room lists (in the app's *Online* screen and Azahar's *Browse Public Game Lobby*), and scripts or Docker for dedicated rooms (`mh4u-server`, `SERVER.md`).
- **Frame rate**: 60, 120 or your monitor's rate (`fps = 120`); the game keeps its normal speed.
- **Widescreen drawn at full width**: 16:9, 21:9 and 32:9, with the hunting HUD at the screen's edges; 16:10 and 4:3 without distortion.
- **The touch screen** as an overlay in a corner of a full-window game (*View → Screen Layout → Custom Layout*); in quests just the map, which you can move and resize (the app's *Edit HUD layout*), with the target camera panel in a view of its own so you can see whom you target; the kick, the target camera and the *Skip* after a quest on buttons, with a hint when you can skip.
- **No dither grid** at high internal resolutions (`dither = off`), and **no second-eye pass**: the game draws the top screen twice (stereoscopic 3D) even with the 3D slider at 0, which doubled and softened the picture; `stereo_3d = off` (the default) draws it once.
- **Multiplayer** (Gathering Hall) over Azahar's network rooms: two to four players, also with friends on official Azahar (2125 or later), at different frame rates if they like; the port keeps its players together when a PC stalls, and fixes a drop when two consoles join at the same moment.
- Everything Azahar has: internal resolution, texture packs, controllers, mods (code mods are built into the library).

## Multiplayer in short

In the app: Esc → *Online* → *Host a room*, *Copy* the invite and send it; your friends *Paste invite* → *Join*, also with the game already running, then go to the Gathering Hall's local play. With a lobby server (yours or a friend's, `SERVER.md`), the app's *Online* screen and the menu list its rooms. Without one, one player hosts: *Multiplayer → Create Room*, type *Unlisted*, with UDP port 24872 forwarded on the router (or everyone in one virtual network: Tailscale, ZeroTier, Radmin VPN). The others use *Multiplayer → Direct Connect to Room*. Everyone needs their own name in *Emulation → Configure → System → Username*. In the app, the same is `host = 24872` or `join = <the host's address>`, and `nickname`, in `mh4u-app.ini`.

## Known limits

- The app is new: it can't install other CIAs or change Azahar's own settings (Azahar, in the same package, does). Earlier versions of it were tried on Windows and in the cloud (Vulkan and OpenGL), less than Azahar; 0.1.6 hasn't been tried with the game yet.
- Save states don't work with the recompiled code: save in the game.
- Cheats that rewrite code don't apply (ones that change data do).
- The macOS build hasn't been tried on a Mac yet.
- The European version (with its v1.1.0 update) has been tried only by the contributor who added it, on Linux, from the title screen to town: not yet on Windows, macOS or Android, in a quest above 60 fps or in a room, and whether European and USA players can play in one room isn't known yet. A code mod that changes the USA update's code breaks it, and one that changes only data hasn't been tried with it.
- Android: the port runs inside Azahar's Android app, with its touch controls and settings; the PC app's launcher and menu aren't there, so the port's options are in `mh4u.ini`.
- Azahar doesn't draw the 3DS GPU's "gas" mode (a few smoke effects).

## Source

GPL-3.0-or-later. The source: this repository at this release's tag (`@COMMIT@`), and Azahar at commit `@AZAHAR@` with `port/azahar/azahar.patch`. Full guide: [docs/PLAYING.md](@REPO@/blob/@COMMIT@/docs/PLAYING.md).
