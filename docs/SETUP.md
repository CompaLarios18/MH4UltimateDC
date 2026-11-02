# Setup: getting your own files

You need two files from **your own** copy of the game:

| File | What it is |
|---|---|
| `exheader.bin` | NCCH extended header (0x400 or 0x800 bytes). Describes where each code segment is loaded in memory. |
| `code.bin` | The game's executable code from ExeFS (`.code`). Retail titles usually ship it BLZ-compressed; our tooling decompresses it automatically. |

## 1. Dump your cartridge / installed title (decrypted)

On a 3DS with custom firmware, **GodMode9** can dump a game card or an installed title as a *decrypted* `.3ds` / `.cxi`. Refer to the GodMode9 documentation and the 3DS homebrew guides for the exact steps for your setup. Dump the **base game**; updates (patches) are separate titles and can be handled later.

Only dump games you own.

## 2. Extract the exheader and ExeFS

Use the bundled extractor. It reads decrypted `.3ds`/`.cci`, `.cia` and `.cxi` files, and refuses encrypted ones (it never handles keys):

```bash
# The update CIA (USA v1.3.0) carries the complete code of the final version: this is what we decompile.
python3 tools/extract_ncch.py "<update>.cia" -o baserom/update_v130

# The base cartridge is only needed for assets (RomFS), later.
python3 tools/extract_ncch.py "<game>.trim.3ds" -o baserom/base --romfs
```

Each output folder gets `exheader.bin`, `code.bin` (still compressed, which is fine), `exefs.bin` and `info.txt`.

> **Which code to use:** a 3DS update replaces the whole ExeFS, so the update title's `code.bin` is the full executable of the patched game. The project targets **USA v1.3.0** (see `config/versions.json`): `tools/recompile.py` and the research tools take its code, since their tables in `config/` hold its addresses. Update RomFS files are applied on top of the base RomFS.
>
> **EUR:** the European **v1.1.0** update (title `0004000E00126100`) is the same program as USA v1.3.0 with its functions a few bytes further along: `.rodata`, `.data` and `.bss` are laid out the same. `config/` keeps USA addresses only; `config/shifts/eur_v110.txt` says where each part of USA's `.text` sits in EUR's, and `tools/build_game.py` applies it, so a EUR player needs only the EUR dump. Europe's code library is built with `build_game.py`, not with `recompile.py`.
>
> Code EUR changed has no address in the shifts, so `build_game.py` leaves out the entry points in it (it prints how many): 45 today, 43 from `config/functions.tsv` and two from `config/extra_entries.txt`. The recompiler finds those functions again only through direct calls and the code addresses it finds in the data and literal pools ([RECOMPILER.md](RECOMPILER.md#pipeline)). The two from `extra_entries.txt`, `0x00103a9c` and `0x00119ec8`, are listed only because a function pointer reaches them at run time: on Europe they run in the interpreter when that happens (the log says `interpreting ...`).
>
> A hook or tunable needs a EUR address too: `build_game.py` stops Europe's build when one has none, and `tests/test_build_game_versions.py` fails in CI first. `python3 tools/version_map.py translate config/shifts/eur_v110.txt 0x...` says whether an address maps (`unmapped` when it doesn't), without a dump. An address inside one of the functions EUR changed can't be mapped at all: put the hook on a point the shifts cover. Only someone with their own dumps of both updates can rebuild the shifts, and never from a dump that isn't from their own copy. A rebuild from the same dumps and the same `config/functions.tsv` writes the same file, so it helps only once `functions.tsv` lists the hook's function. With the USA update extracted as above:
>
> ```bash
> python3 tools/extract_ncch.py "<EUR update>.cia" -o baserom/eur_update_v110
> python3 tools/version_map.py build --name "EUR v1.1.0" -o config/shifts/eur_v110.txt \
>     --src-exheader baserom/update_v130/exheader.bin --src-code baserom/update_v130/code.bin \
>     --dst-exheader baserom/eur_update_v110/exheader.bin --dst-code baserom/eur_update_v110/code.bin
> ```
>
> Addresses in docs, comments and `config/` stay USA v1.3.0's; `python3 tools/version_map.py translate config/shifts/eur_v110.txt 0x...` gives the EUR one.

`baserom/` is git-ignored. **Never commit it.**

## 3. Record which version you have

Run:

```bash
python3 tools/verify_dump.py baserom/update_v130/exheader.bin baserom/update_v130/code.bin
```

It prints the title ID, region-relevant fields and SHA-1 hashes. If your version isn't listed in `config/versions.json`, open an issue with that output (hashes only — **never upload the files**).

## 4. Load into a disassembler

```bash
python3 tools/codebin2elf.py --exheader baserom/update_v130/exheader.bin --code baserom/update_v130/code.bin -o build/mh4u_v130.elf
```

Open `build/mh4u_v130.elf` in **Ghidra** (processor: `ARM:LE:32:v6`, the 3DS's ARM11 cores are ARMv6K). Segments `.text`, `.rodata`, `.data` and `.bss` will be mapped at their real runtime addresses.
