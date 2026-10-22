# The Azahar app's smoke test (`mh4u`, on the reference PC)

The maintainer's procedure for every work package that changes a file under `port/app/` that the Azahar app `mh4u` compiles (N2b rule 1). It starts from the Windows package of the work package's branch and walks the app from the launcher to quitting, with the steps that exercise what the package moved. It is about **50 minutes** (40 without the two-instance room, step 14). The packages after N2b-01 (N2b-02 first) add their own steps at the end, under their own heading.

It tests `mh4u` (SDL2, Azahar's core). `mh4u-native` has its own procedure, *Setting up mh4u-native on the reference PC* in `docs/native/app.md` (N2b-11).

**What N2b-01 changed, so you know what to look at.** The in-game menu now draws over an `App::IWindow` instead of Azahar's window class, `IRuntime` lost `HostSystem()` (the windows are made from the Azahar runtime directly), the hint after a quest asks `IRuntime` who the skip waits for, the text of a new `mh4u.ini` lives in `port/src/options_template.cpp`, and the launcher's Settings screen asks `IRuntime` for the options file's path. Nothing should look or behave differently: any difference from how the app was before this branch is a failure to report.

## What you need

- The Windows package of the branch: dispatch `windows.yml` on it (the orchestrator does this and gives you the run), download the artifact `mh4u-port-windows`, unpack it in a new folder.
- Your own decrypted game, the v1.3.0 update `.cia`, and the code library already built for them (or let the launcher's Setup build it, first time only: several minutes). Your save is on the user folder (`%APPDATA%\Azahar\`, or `user\` next to `mh4u.exe` if there is one): copy it before the test if you want to keep it as it is.
- A keyboard and mouse; a controller for steps 9 and 10 (any one the app maps on its own).
- For the room (step 14): a second instance, either on a second PC that reaches the first, or on the same PC with a copy of the user folder (step 14 says how).

## The procedure

Times are rough. Stop at the first step whose result is not as written, and send what the last section says.

1. **Start the launcher** (1 min). Double-click `mh4u.exe`. *Expect:* the launcher opens on its home screen (or Setup the first time). Open **Settings**: the line under "Settings" names `mh4u-app.ini` and, after "The game's own options ... are in", the full path of `mh4u.ini` in the user folder. *Fails if:* the app does not open, closes at once, or that path is empty or wrong.
2. **A new options file** (2 min). Close the app. In the user folder rename `mh4u.ini` to `mh4u.ini.old` (the option file only; leave `mh4u-app.ini`). Start `mh4u.exe` again and go to **Play**. Quit once the game window is up (step 15 shows how; Alt+F4 will do here). *Expect:* a new `mh4u.ini` exists and starts with `# MH4U PC port options. Changes apply the next time the game starts.`, with `fps = 60`, `widescreen = 16:9`, `touch_overlay = top-right 30 85` and `net_sync = on` among its lines. Put `mh4u.ini.old` back afterwards if you want your own options.
3. **Play** (3 min). Launcher → **Play** (Enter). *Expect:* the game window opens (full screen unless your settings say otherwise), the title screen and the village appear and the game runs at its usual speed.
4. **The menu opens and takes the keys and the mouse** (2 min). Press Esc. *Expect:* the in-game menu with the tabs Game, Display, Audio, Controls, Multiplayer, Quit; the game stops reacting to your keys and mouse (a held button reads released). Move over the tabs with the mouse and with the arrow keys and Enter; click a row; scroll. With the menu still open, resize the window (or press F11 and back): the menu's text and widgets grow and shrink with the window, and stay sharp (not stretched). Press Esc to close it. *Fails if:* the menu does not open, ignores the mouse or keys, or does not scale with the window.
5. **Touch screen is let go when the menu opens** (1 min). In the game hold the left mouse button on the touch screen (hold Alt first if the mouse is the camera), then press Esc while still holding it. *Expect:* the menu opens and, after you let go and close it, the game does not see a stuck stylus (the touch screen does not keep a finger pressed).
6. **Change an option in each tab** (6 min). With the menu open, make one change in each of these and note what it should do. Each is applied at once (the game's options that need a restart excepted; these do not), and saved when you close the menu; *Expect* the result in the third column, and after closing the menu the file in the fourth changed.

   | Tab | Change | Expect now | Saved in (when the menu closes) |
   |---|---|---|---|
   | Game | *Frame rate* from 60 to 30 and back to 60 | the counter of step 7 follows, the game's speed stays right | `mh4u.ini` (`fps`) |
   | Display | *Smooth scaling* off, then on | the picture sharpens, then smooths | `mh4u-app.ini` |
   | Audio | *Volume* to 0, then back to your usual | the sound stops, then returns | `mh4u-app.ini` |
   | Controls | *Mouse sensitivity* up one step, then back | the camera turns faster with the mouse, then as before | `mh4u-app.ini` |
   | Multiplayer | open the tab | the status line and *Join a room* / *Host a room* show, no error | n/a |
   | Quit | open the tab, then choose not to quit | the "Quit?" box closes, the game goes on | n/a |

   After the Controls change, move the camera with the mouse (the input profile is rebuilt when the menu closes). *Fails if:* a change does nothing, crashes the app, or the file is not updated once the menu is closed.
7. **The frame-rate counter** (1 min). Press F3. *Expect:* a counter appears in a corner with the game's frames per second, and F3 again hides it. In **Display** the row *Frame-rate counter* shows the same state and changes it.
8. **Full screen** (2 min). Press F11, then F11 again; Alt+Enter, then Alt+Enter. In the menu's **Display** tab toggle *Full screen* with the mouse. *Expect:* the window switches between full screen and a window every time (the picture refits at once, the mouse still works as the stylus in both), and the menu's checkbox follows. *Fails if:* a toggle is ignored, the picture stays the old size, or the app freezes.
9. **A controller** (3 min). Connect a controller. *Expect:* the game takes it. Press its **Guide** button: the menu opens; Guide again: it closes. Press **Back and Start** together: the menu opens; again: it closes. Move through the menu with the stick and A/B. *Fails if:* the menu opens twice, or opens on B or A being pressed while the game is running (that is a key echo of the controller and must not open it).
10. **A controller comes and goes** (2 min). With the game running, unplug the controller and plug it back in. *Expect:* the game keeps running; when it is back the controller plays again, with no restart. (In the menu's Controls tab, changing a binding applies the same way.)
11. **The keyboard and the clipboard** (4 min). Start a new save, or any place that asks for a name, until the game asks you to name a character. *Expect:* the app's own keyboard opens over the game (not Azahar's). Type a name with the keyboard; use Backspace; paste a text you copied beforehand with Ctrl+V (*Expect:* it appears, cut to the length the game allows); confirm with Enter or the OK button. The name appears in the game. *Fails if:* a key does nothing, the paste adds nothing, or the name does not arrive in the game.
12. **The key guides** (1 min). Press F1. *Expect:* the key guides show over the game while you play with the keyboard and mouse, in a corner; F1 hides them. Take the controller and move its stick: with `input_mode` on auto the guides go; use the mouse: they come back.
13. **The skip hint after a quest** (8 min). Complete a quest (any, solo is enough). *Expect:* once it is complete, lines at the bottom of the screen say which keys do what (the skip button, and the buttons held to end the wait, with a progress while you hold them); they go when the wait ends. Online with a second console using the port (step 14 and a quest together): while you hold the end buttons and the other player has not, the line names who is waited for. *Fails if:* no lines appear, or they stay after the quest, or the app stops.
14. **Host a room and join it from a second instance** (10 min).
    1. *Host:* Esc → **Multiplayer** → **Host a room** → *Host this room* (port 24872, no password). *Expect:* the tab says you host and shows an invite like `mh4u://...:24872`.
    2. *Second instance:* on a second PC with the same package and setup: `mh4u.exe --windowed --join <the first PC's address>:24872`. On the same PC instead: copy the user folder to `second-user`, then from a console `mh4u.exe --user second-user --new-console-id --windowed --join 127.0.0.1:24872` (a room lets in one player per console ID).
    3. *Expect on both:* the Multiplayer tab lists two members; *Local play heard* leaves "not yet" once both are in the village or a hall; with the game's Gathering Hall → Local Play one finds the other's hall.
    4. *Leave the room* on the second instance and *Close the room* on the first. *Expect:* both tabs return to no room, no error.
15. **Quit** (1 min). Esc → **Quit** tab → *Quit the game* → *Quit* in the "Quit?" box. *Expect:* the game window closes, the app ends within a few seconds and no process of it stays (check Task Manager).
16. **The log** (2 min). Open `%APPDATA%\Azahar\log\mh4u_log.txt` (or `log\` in the user folder). *Expect:* it starts with `MH4U PC port app, Azahar ...`, has `renderer: Vulkan` (or the renderer your settings choose), `in-game menu: Esc, or a controller's Guide or Back+Start`, a `frame-rate counter: <corner>` or `frame-rate counter: off` line for each F3 of step 7, and no line marked `<Error>` or `<Critical>` apart from any that a step above caused on purpose.

## What to send back

In the batch PR or the chat, **no screenshots, captures or save files**; only text:

- For each numbered step: `pass`, or `fail` with one line of what you saw instead.
- The **first 3 lines and every `<Error>` / `<Critical>` line** of `mh4u_log.txt`, and the number of lines it has.
- The package's run number (the `windows.yml` run you tested) and whether you used a copy of your save.
- Step 2 and 6 only: the first line of the new `mh4u.ini`, and the `fps` value in it after step 6.

## When a step fails

Stop. Send the number of the step, what you saw, and **the last 50 lines of `mh4u_log.txt`**. Do not retry past it: the steps after it depend on it. If the app does not start at all, send the last 50 lines of the log and, if there is no log, say so (the app writes it from its first line). A crash with no log: send the Windows error text only.
