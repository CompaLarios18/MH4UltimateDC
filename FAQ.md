# FAQ

Short answers to the questions people ask most. The full guide is [`docs/PLAYING.md`](docs/PLAYING.md). If your question isn't here, open an issue.

## The project

**So it's just Azahar?**
The hardware side (system services, graphics, audio, network rooms) is Azahar's core, credited and GPL-3.0. The game's code is not emulated: it's recompiled to C and runs natively on your PC. That is what makes source-level fixes, 120 fps and code-cave mods possible, and it lets us replace more pieces over time. It's the same architecture as Zelda64Recomp. Details: [`docs/PORT_ARCHITECTURE.md`](docs/PORT_ARCHITECTURE.md).

**Is this legal?**
No game data is distributed: no ROM, no `code.bin`, no RomFS, no keys. You build everything from your own dump, on your own PC. It's the same stance as Azahar and every recompilation project. See [`docs/LEGAL.md`](docs/LEGAL.md).

**Where do I get the game?**
From your own cartridge or eShop purchase, dumped with GodMode9 on your 3DS. Where to get it otherwise is not discussed here.

## Setting up

**What do I need?**
- The game (USA), dumped as a decrypted `.3ds`, `.cci` or `.cxi`, and its v1.3.0 update as a `.cia`. GodMode9 can make both.
- A PC with Windows 10/11 x64 or Linux x64, or a Mac with Apple silicon (not tried yet). To play on Android you still need a PC once, to build the game's code (see *Android?* below).
- Python 3.10 or newer, which the setup uses to build the game's code.

The downloads are on the [releases page](https://github.com/CompaLarios18/MH4UltimateDC/releases). Details: [What you need](docs/PLAYING.md#what-you-need).

**Does it work with the European or Japanese version?**
Not yet. Only the USA version with the v1.3.0 update (title `0004000000126300`) is supported.

**The setup says my update is v0.0.1 (or v1.0.0), not v1.3.0**
Some tools that decrypt or rebuild a `.cia` reset the version label in it and leave the update itself as it was. Releases after 0.1.5 go by the update's code, as the code library's build always did: such a v1.3.0 `.cia` is used as v1.3.0, and the setup says what it found. If the setup still turns it away and says that its code isn't v1.3.0's either, the `.cia` holds another update, or a changed one. With release 0.1.5 or older, build the `.cia` with GodMode9 from the update installed on your 3DS, and install that one.

**Why does the first start take a while?**
The setup recompiles the game's code from your update and then compiles it for your PC: a couple of minutes to recompile, several more to compile. It happens once, and again only after you add, change or remove a code mod.

**Windows says "Bad Image ... 0xc0e90002"**
That's Smart App Control blocking unsigned files (Azahar's DLLs, the compiler, the game's code library). Turn it off in *Windows Security → App & browser control → Smart App Control settings*. Since the April 2026 update, it can be turned back on later without reinstalling Windows.

**"The game stopped" right after Play**
The game itself gave up as it started, which can mean it couldn't read its own files: a mod, a damaged dump, or a game file that isn't decrypted properly. If mods are on, turn them off in the launcher's *Mods* and try again. Otherwise dump the game again with GodMode9 on your 3DS and choose the new `.3ds`, `.cci` or `.cxi` in the launcher's *Setup*, and install the v1.3.0 update again from a `.cia` that GodMode9 made (with `launcher = off` in `mh4u-app.ini`, start the app with `--launcher` to reach them). If it still stops, open an issue with the log (see *How do I report a bug?* below). Details: [Troubleshooting](docs/PLAYING.md#troubleshooting).

**Can I keep my 3DS progress?**
Yes. Back up the save on your console with Checkpoint, then copy its files into the game's save data folder while the game isn't running. Steps: [Your 3DS save](docs/PLAYING.md#your-3ds-save).

## Playing

**My PC shows 75 % speed**
That's measured emulation speed, not a setting: your PC isn't keeping up with the frame rate you asked for. Lower the `fps` target or the CPU clock in *Esc → Game* until it reads 100 %.

**Do save states work?**
No. The game's code runs natively, and its call stacks live on your PC's own stacks, which a save state can't capture. Save in the game. Cheats that change data work; cheats that rewrite code don't.

**Pause and fast forward?**
In the port's app, playing alone: **P** pauses, **Tab** runs the game at 2x (the speed and the keys are in the menu, Esc). In a room they're off, since the others' games run on with yours.

**In a quest the whole touch screen stays in the corner, and the map settings do nothing**
The map view shows the map panel of your touch screen's panel layout. Without one (*Customize Panels* in the quest menu), the corner keeps the whole touch screen, so the map's size and place, *Edit HUD layout* and F12 change nothing: put the map panel back. The log says so once a quest, and the app says why when F12 has nothing to switch (this, the touch screen not over the game in a corner, or *In quests, only the map* off). With the map panel there, F12 or the right stick's click may have switched quests to the whole touch screen: press it again. Details: *The bottom screen* in [Options](docs/PLAYING.md#3-options).

**Keys for the touch screen (an item shortcut, the target camera)?**
In the port's app: touch taps. `touch_tap_1 = Z @ 52 220` in `mh4u-app.ini` touches the touch screen at that point (x from its left edge, y from its top, in its 320x240 pixels) while Z is held; up to 8. Details: [Or play with the port's app](docs/PLAYING.md#2b-or-play-with-the-ports-app).

**Steam Deck?**
There's a Linux build (`mh4u.AppImage`), not tried much on a Deck yet. In Game Mode a desktop's file dialog may not show, or fails (a "Service crash"), so the launcher chooses files in its own browser there: the D-pad, A and B, with your SD card under *Drives*. Elsewhere *Browse in the launcher* opens it. If you try the port on a Deck, open an issue and tell us how it went.

**Mac?**
There's a build for Apple silicon (Vulkan through MoltenVK), not tried on a Mac yet. Reports are welcome in an issue.

**Android?**
Yes, from release 0.1.5: `mh4u-port-android.apk` is Azahar's Android app with the port inside, for arm64 devices with Android 10 or newer. The game's code library is built on a PC from your own v1.3.0 update and copied to the device: [steps](docs/PLAYING.md#7-android-experimental). It's new, so reports are welcome in an issue.

## Online

**Can I play with people on regular Azahar?**
Yes. Rooms are compatible with official Azahar (2125 or later), and players don't need the same frame rate. Everyone needs the v1.3.0 update. Join a Gathering Hall one at a time: official Azahar gives two consoles that join in the same instant the same id, and the game drops one of them.

**Do I need to open ports?**
Only to host for friends over the internet. Either forward UDP port 24872 on your router to your PC, or put everyone in one virtual network (Tailscale, ZeroTier, Radmin VPN), which needs no forwarding. Joining needs nothing, and neither does playing on one network (the same Wi-Fi): the others find the host's room under *On this network*. UPnP is planned. Details: [Multiplayer](docs/PLAYING.md#4-multiplayer).

## Mods

**The HD texture pack download fails**
The pack comes straight from its author's Dropbox, which pauses shared links for 24 hours after heavy traffic. Try again later: the download resumes where it stopped. And thank mhondeck for the pack.

## Bugs

**How do I report a bug?**
Open an issue, say what you did and what happened, and attach `mh4u_log.txt` from Azahar's log folder: `%APPDATA%\Azahar\log` on Windows, the `log` folder inside Azahar's user folder elsewhere. Close the game first: the log is written in a buffer, and it's overwritten the next time the game starts.

**The launcher can't build the code library**
The line under the build's progress bar says how it ended, and *Copy the output* copies all the build printed, with the app's version: paste it in an issue (the log has its last lines too). Read it first: when a file didn't compile, the compiler quotes the lines of recompiled code it stopped at, and that code is made from the game's, so leave those lines out (of the log too). Meanwhile the package's `build-code-library.sh` runs the same build from a terminal on Linux (`Build code library.command` on a Mac, `Build code library.bat` on Windows).
