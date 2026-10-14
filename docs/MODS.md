# Mods

Azahar loads a game's mods from one folder, `load/mods/0004000000126300/` in its user folder (right-click the game → *Open Mods Location*; the v1.3.0 update uses the base game's folder). Many mods at once means merging them by hand into that one folder, and two mods that replace the same file silently overwrite each other there.

`tools/mods.py` does that merging. It keeps a **mod library**, one folder per mod, in an order in which later mods win, shows which files two mods both change, and puts the enabled mods' files into Azahar's folder for you. It never touches files it didn't put there unless you tell it to. It uses only Python's standard library, like the other tools, and works the same on Windows, Linux and macOS.

The port's launcher will drive it later (each command has a `--json` output for that); for now it is a command:

```bash
# Linux, macOS (from the repository, or the package's mh4u-tools folder)
python3 tools/mods.py list

# Windows
py tools\mods.py list
```

Folders: the library is `mh4u-mods/` in Azahar's user folder (`%APPDATA%\Azahar` on Windows, `~/.local/share/azahar-emu` on Linux, `~/Library/Application Support/Azahar` on macOS). `--user-dir DIR` points at another Azahar user folder (a portable setup), `--library DIR` at another library.

## Installing mods

```bash
python3 tools/mods.py add "Some mod 1.0.zip"     # a .zip, unpacked into the library
python3 tools/mods.py add path/to/some-mod        # or a folder, copied into it
python3 tools/mods.py list                        # the mods, on or off, in order
python3 tools/mods.py deploy                      # put the enabled ones in Azahar's mods folder
```

Then start the game. Nothing changes in Azahar's folder until `deploy`; `list` and `status` say when it is out of date.

- `add PATH|ZIP` puts a mod in the library, turned on and last in the order (so it wins over the others). Its name is the `name` in its `mod.json`, else the folder's or the zip's name; `--name` chooses another. `--disabled` adds it turned off. Adding a mod of the same name again is refused (capital letters don't count: on Windows and macOS `Mod` and `mod` are one folder); `--replace` updates it in place instead, keeping its place in the order and whether it is on.
- `enable NAME`, `disable NAME` turn a mod on and off; `remove NAME` deletes it from the library. NAME is its folder in the library or its `mod.json` name, in any capitals.
- `order NAME POSITION` moves a mod: `1` (or `first`) is first, the number of mods (or `last`) is last. **Later mods win.**
- `deploy` puts the enabled mods into Azahar's mods folder, `undeploy` takes them out again (for a game without mods; turning nothing off).
- A folder you put into the library yourself also counts: it shows in `list` as new and off, and `enable` turns it on.

Only `.zip` archives are unpacked; unpack a `.7z` or `.rar` yourself and add the folder. A zip is refused when one of its paths would land outside the mod's folder (`..`, an absolute path, a drive letter), when a name isn't allowed on Windows (`aux.arc`, `a:b`), when two names differ only in capitals, or when it unpacks to more than 8 GB (4 GB per file). Symbolic links in it are skipped. Password-protected zips aren't supported.

## What a mod can look like

Mods come in a few layouts, and none of them needs a `mod.json`:

- **Azahar's own layout**, what you would put in *Open Mods Location*: `romfs/` with files that replace or add game files, `romfs_ext/` with `<file>.stub` (deletes that file) or `<file>.ips`/`.bps` (patches it), `exefs/` (or the top of the mod) with `code.bin`, `code.ips`, `code.bps`, `banner.bnr`, `icon.bin`, `logo.bcma.lz`, and `exheader.bin`.
- **A title folder**: the same inside `0004000000126300/` (or the update's `0004000E00126300/`), or inside `luma/titles/0004000000126300/`, the layout of mods made for a 3DS with Luma3DS. A mod with folders only for other games or regions is refused: the port plays MH4U USA.
- **The game's own folders**, without `romfs/` around them: `data/...`, or a language folder such as `eng/data/...`. They go into `romfs/`.
- Any of these inside one wrapping folder (`Some mod v1.0/romfs/...`), as zips often have, perhaps with a readme next to it.

Readmes, pictures and the like are left out. Names are matched without regard to capitals (`RomFS/` is `romfs/`), and the game's folders are given their own spelling (`Data/` becomes `data/`), since the game finds its files only by their exact names. Where Azahar would take only one file of a group, the mod manager deploys only that one and says so: `exefs/code.ips` before `exefs/code.bps`, `code.ips` and `code.bps`; `exefs/banner.bnr` before `banner.bnr`.

One limit of Azahar: it reads only the files **at the top of `romfs_ext/`** (layered_fs.cpp looks at that folder without going into its sub-folders), so a `.stub` or patch for a file in a folder (`romfs_ext/data/x.arc.ips`) does nothing, and the mod manager leaves it out with a warning. MH4U's archives are in folders (`data/`, `eng/data/`), so a mod for one of them has to ship the whole changed file in `romfs/` instead.

## Order and conflicts

Two enabled mods conflict when they change the same game file. The later one in the order wins, and its file is the only one deployed. A replacement in `romfs/` and a patch or `.stub` for the same file in `romfs_ext/` count as the same file, as do `exefs/banner.bnr` and `banner.bnr`.

```bash
python3 tools/mods.py conflicts          # each file changed by more than one enabled mod, and the winner
python3 tools/mods.py conflicts --all    # the same among all the mods, on or off
python3 tools/mods.py order "English menus" last
```

**Capitals.** `data/example.arc` and `data/EXAMPLE.arc` are one file on Windows and macOS, and the mod manager treats them as one everywhere, so the result is the same on every system: one conflict, the later mod's file, with its spelling. When mods spell a folder differently, the later mod's spelling is used and `list` warns, because the game's names are case-sensitive and only its own spelling works.

## Deploying

`deploy` makes Azahar's mods folder hold exactly the enabled mods' files:

- It **hard-links** each file to the library (no copy, no extra disk space) when it can, and copies it otherwise (another drive, a FAT or exFAT drive; `--copy` always copies). A hard link is the same file in two places: editing a deployed file edits the library's.
- It remembers what it put there in `mh4u-mods-deployed.json` in that folder. Later deploys remove or replace only those files, and only while they are as deployed; a file you changed since is left alone and from then on counts as yours.
- **Files you put there yourself are kept.** When one of them is in the way (a mod would replace it, or Azahar would use it instead of a mod's: a `.stub` for the same file, another code patch), `deploy` changes nothing and lists them: move them into a mod, or `deploy --force` to replace them.
- It is quick when nothing changed, and running it twice changes nothing the second time. `deploy --dry-run` only says what it would do; `status` says the same, and more.

To keep mods you already installed in Azahar's folder by hand, make them a mod: `add` that folder (`--name "My old mods"`), then `deploy --force`, which replaces the hand-made files with the library's.

Deploy while the game isn't running: it reads the mods' files as it plays, and on Windows a file can't be replaced while it is open.

## Code mods and the code library

Code mods (`code.bin`, `code.ips`, `code.bps`) change the game's code. Azahar applies only one code patch, on top of at most one `code.bin`, so **only one code mod can be on at a time**: `enable` refuses a second one and names the first, and `add` adds a second one turned off.

The port runs the game's code recompiled for your PC (the code library, [PLAYING.md](PLAYING.md) step 1), and it runs it only when it is built from the same code that Azahar loads. So:

- A code mod that only changes the game's **data** (such as the crafting patch of [anco's MH4U quality-of-life project](https://codeberg.org/anco/mh4u_qol)) works as it is.
- One that changes the game's **code** needs the code library built again with it in place: deploy, then build (`tools/build_game.py --install`, or the package's build script, which apply the code mods in Azahar's folder). Build again after adding, changing or removing such a mod. Without that, the port runs the game on Azahar's own CPU emulation and says why when the game starts.

`list` shows for each code mod whether it changes the code or only data, read from the patch itself (the offsets it writes, compared with the size of the code segment in `config/versions.json`); for a whole `code.bin` that can't be told without the game's code. `status` (and `deploy`) says whether the code library needs building again:

| `status` says | Meaning |
|---|---|
| `ok` | built with the code mods now in Azahar's folder, or both those and the ones it was built with change only data |
| `rebuild` | the code mods changed since it was built: build it again |
| `unknown` | it was built without a record of its code mods (by an older `build_game.py`, or the record is from another build) |
| `missing` | no code library: [PLAYING.md](PLAYING.md) step 1 |

The record is `recomp/0004000000126300.build.json` next to the library, written by `build_game.py --install`. `status` also notices Luma's code patch (`sdmc/luma/titles/0004000E00126300/code.ips`), which Azahar applies when the mods folder has none but `build_game.py` doesn't: move it into a mod instead.

## For mod authors

The simplest mod to install is a zip of Azahar's layout with a `mod.json` next to it:

```
My Mod 1.0.zip
  mod.json
  romfs/data/example.arc
  readme.txt
```

`mod.json` says what the mod is; every field is optional:

```json
{
  "name": "My Mod",
  "version": "1.0",
  "author": "you",
  "description": "What it changes, in a sentence or two.",
  "url": "https://example.org/my-mod",
  "contains": ["romfs"]
}
```

- `name` is how players see it and name it in commands; the library folder is named after it.
- `contains` lists what the mod has, out of `romfs` (files that replace or add game files), `romfs_ext` (patches or deletions of game files), `code` (`code.bin`, `code.ips`, `code.bps`), `exefs` (`banner.bnr`, `icon.bin`, `logo.bcma.lz`) and `exheader`. The mod manager reads the files themselves and warns when they don't match it.
- `root` names the folder inside the mod that holds its files, for a layout it doesn't recognise by itself (`"root": "files"`).

Make code patches for the USA v1.3.0 update's `code.bin` (the port's code), and say in the description whether they change code or only data.

## For the launcher (`--json`)

With `--json` (before or after the command) each command prints one JSON object: `"ok": true` and `"command"` with the result, or `"ok": false` and `"error": {"code", "message", ...}` (exit status 1; 2 for a usage error). `list`, `add`, `remove`, `enable`, `disable` and `order` return the library: `mods` (in order; each with `id`, `name`, `version`, `author`, `description`, `url`, `position`, `enabled`, `new`, `layout`, `contains`, `files`, `code`, `wins`, `loses`, `path`, `warnings`, `errors`), `conflict_count`, `deploy_state` (`up_to_date`, `out_of_date` or `blocked`), `problems`, `warnings` and, for the commands that change it, `messages`. `conflicts` returns `conflicts` (`path`, `mods`, `winner`, `files`); `deploy`, `undeploy` and `status` return `deploy` (`state`, `add`, `update`, `remove`, `in_the_way`, `changed`, `foreign`, `problems`) and `code_library` (`state`, `reason`, `code_files`, `changes_code`). Error codes include `not_found`, `exists`, `layout`, `zip`, `code_mod_conflict`, `foreign_files` and `io`.
