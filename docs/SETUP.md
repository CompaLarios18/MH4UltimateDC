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

Use an open-source NCCH tool such as **3dstool** or **ctrtool** to split the decrypted image. Conceptually:

1. From the `.3ds`/`.cci`, extract **partition 0** (the main executable content, a `.cxi`).
2. From the `.cxi`, extract the **extended header** and the **ExeFS** (and the RomFS, which you'll want later for assets).
3. From the ExeFS, extract **`.code`** (usually written out as `code.bin`).

Check each tool's `--help` / README for the exact flags of the version you install. *(Contributors: once someone has a verified command line, please PR it here.)*

Copy into the repository:

```
baserom/exheader.bin
baserom/code.bin        # exefs/code.bin (compressed is fine)
```

`baserom/` is git-ignored. **Never commit it.**

## 3. Record which version you have

Run:

```bash
python3 tools/verify_dump.py baserom/exheader.bin baserom/code.bin
```

It prints the title ID, region-relevant fields and SHA-1 hashes. If your version isn't listed in `config/versions.json`, open an issue with that output (hashes only — **never upload the files**).

## 4. Load into a disassembler

```bash
python3 tools/codebin2elf.py --exheader baserom/exheader.bin --code baserom/code.bin -o build/mh4u.elf
```

Open `build/mh4u.elf` in **Ghidra** (processor: `ARM:LE:32:v6`, the 3DS's ARM11 cores are ARMv6K). Segments `.text`, `.rodata`, `.data` and `.bss` will be mapped at their real runtime addresses.
