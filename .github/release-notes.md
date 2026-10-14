**Monster Hunter 4 Ultimate on PC**, through Azahar's 3DS core with the game's own code recompiled for your CPU, plus PC options: higher frame rates, widescreen up to 32:9, the touch screen over the game, and multiplayer over the internet.

🇲🇽 *Port de MH4U para PC: 60/120 fps, pantalla ancha, la pantalla táctil encima del juego y multijugador por internet. Necesitas tu propia copia del juego y su actualización v1.3.0.*

## No game data

These packages contain **no game code or data**. You need your own decrypted copy of Monster Hunter 4 Ultimate (USA) and its **v1.3.0 update** (`.cia`). The game's code library is built on your computer from your update, in one step.

## Downloads

| System | File | Status |
|---|---|---|
| Windows 10/11 (x64) | `mh4u-port-windows.zip` | played for hours, hunts included, at 120 fps |
| Linux (x86-64, Steam Deck too) | `mh4u-port-linux.tar.gz` | an AppImage; tried on Ubuntu 24.04 |
| macOS (Apple silicon) | `mh4u-port-macos.zip` | builds; not tried on a Mac yet |

## How to play (Windows)

1. Install [Python](https://www.python.org/downloads/) 3.10 or newer (tick *Add python.exe to PATH*).
2. Unzip, double-click `Build code library.bat` and choose your v1.3.0 update `.cia`. It builds the game's code library (the first time it downloads the C compiler, about 180 MB) and installs the update in Azahar.
3. Start `azahar.exe`: *File → Load File…* → your game.

Options (frame rate, widescreen, the touch screen…): *Tools → MH4U Port Options…*. If Windows 11's Smart App Control blocks `azahar.exe` (unsigned), it has to be turned off. Linux and macOS: the steps are in `README-mh4u.txt` in each package.

## What's in it

- **Frame rate**: 60, 120 or your monitor's rate (`fps = 120`); the game keeps its normal speed.
- **Widescreen drawn at full width**: 16:9, 21:9 and 32:9, with the hunting HUD at the screen's edges; 16:10 and 4:3 without distortion.
- **The touch screen** as an overlay in a corner of a full-window game (*View → Screen Layout → Custom Layout*); in quests just the map; the kick and the target camera on buttons.
- **No dither grid** at high internal resolutions (`dither = off`).
- **Multiplayer** (Gathering Hall) over Azahar's network rooms: two to four players, also with friends on official Azahar (2125 or later), at different frame rates if they like; the port keeps its players together when a PC stalls, and fixes a drop when two consoles join at the same moment.
- Everything Azahar has: internal resolution, texture packs, controllers, mods (code mods are built into the library).

## Multiplayer in short

Azahar has no public room list, so one player hosts: *Multiplayer → Create Room*, type *Unlisted*, with UDP port 24872 forwarded on the router (or everyone in one virtual network: Tailscale, ZeroTier, Radmin VPN). The others use *Multiplayer → Direct Connect to Room*. Everyone needs their own name in *Emulation → Configure → System → Username*.

## Known limits

- Save states don't work with the recompiled code: save in the game.
- Cheats that rewrite code don't apply (ones that change data do).
- The macOS build hasn't been tried on a Mac yet.
- Azahar doesn't draw the 3DS GPU's "gas" mode (a few smoke effects).

## Source

GPL-3.0-or-later. The source: this repository at this release's tag (`@COMMIT@`), and Azahar at commit `@AZAHAR@` with `port/azahar/azahar.patch`. Full guide: [docs/PLAYING.md](@REPO@/blob/@COMMIT@/docs/PLAYING.md).
