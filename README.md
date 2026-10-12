# MH4UltimateDC

A work-in-progress decompilation of **Monster Hunter 4 Ultimate** (Nintendo 3DS), with the long-term goal of a **native PC port**.

> 🇲🇽 *Proyecto de decompilación de Monster Hunter 4 Ultimate (3DS) con el objetivo de llegar a un port nativo para PC. La documentación está en inglés para abrirlo a la comunidad; las discusiones en español son bienvenidas.*

## Status

🟧 **Phase 2 — Running something on PC.** Target version is **USA v1.3.0**.

- A **static recompiler** translates the whole game (43,000+ functions, 3.4 M ARM/Thumb instructions) into C, generated on your machine from your own dump and verified against an ARM11 model ([`docs/RECOMPILER.md`](docs/RECOMPILER.md)).
- Next: run that code on top of Azahar's 3DS core (kernel, services, GPU, audio) to boot the game natively ([`docs/PORT_ARCHITECTURE.md`](docs/PORT_ARCHITECTURE.md)).
- In parallel, functions get hand-decompiled into readable C where the port needs to change behaviour (widescreen, 60 fps, HUD), each one verified with `tools/difftest.py`.

See [`docs/ROADMAP.md`](docs/ROADMAP.md) for the plan and [`docs/research/first-look-v130.md`](docs/research/first-look-v130.md) for first findings.

## ⚠️ This repository contains no game data

This project **does not** and **will not** include the ROM, `code.bin`, RomFS assets, encryption keys, or any other copyrighted material from Capcom or Nintendo. To build or work on it you must dump **your own** copy of the game. See [`docs/LEGAL.md`](docs/LEGAL.md) and [`docs/SETUP.md`](docs/SETUP.md).

Pull requests that add copyrighted binaries or assets will be rejected and purged from history.

## Quick start

```bash
# 1. Python tooling (3.10+)
pip install -r requirements.txt

# 2. Place your own decrypted dump's ExeFS code + exheader in baserom/  (see docs/SETUP.md)
#    baserom/exheader.bin
#    baserom/code.bin      (compressed or decompressed, both work)

# 3. Produce an ELF for your disassembler
python3 tools/codebin2elf.py --exheader baserom/exheader.bin --code baserom/code.bin -o build/mh4u.elf

# 4. Run the tool tests
python3 -m pytest

# 5. Recompile the game into C (build/recomp/, never committed) and build it
python3 tools/recompile.py -o build/recomp
(cd tools && python3 -m recomp.build ../build/recomp --harness --opt O0 -D RC_CHECK_RETURNS)
```

## Layout

| Path | Purpose |
|---|---|
| `tools/` | Python tooling (BLZ decompression, exheader parsing, code.bin → ELF, difftest, static recompiler in `tools/recomp/`) |
| `config/` | Per-region version info (hashes, title IDs) — filled in as dumps are verified |
| `src/`, `include/` | Decompiled C/C++ source (empty for now) |
| `port/` | Future PC platform layer: 3DS OS services, PICA200 → modern GPU, input, audio |
| `docs/` | Roadmap, setup, legal, research notes |
| `baserom/`, `build/` | **Git-ignored.** Your dump and build output |

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md). Research notes (file formats, struct layouts, function names) are as valuable as code at this stage.
