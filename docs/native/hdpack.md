# The HD pack pre-processor (N2-17)

D14: the HD texture pack is downloaded at setup from its author's own links and converted on the
player's PC, never shipped. `mh4n_hdpack` turns the pack folder into one archive that loads fast:
BC7 with mipmaps, indexed by the hash Azahar names textures by. N2-18 streams textures out of it.

```
mh4n_hdpack PACK_DIR -o OUT [--threads N] [--quality fast|normal|high] [--restart] [--verify]
```

- `PACK_DIR` is the pack in the layout Azahar loads (`load/textures/<title id>/`): texture files
  and an optional `pack.json`. The code is the module `native/src/hdpack`; `native/tools/hdpack`
  holds only `main`.
- Progress is one line at most once a second on stdout, for the setup to show:
  `hdpack: 13/24 textures (54.2%), 17.3 MiB written, 0 failed, 2 s, about 1 s left`.
- Exit status: 0 the archive is written; 2 bad arguments or an unusable pack (no folder, a
  `pack.json` that is not valid); 3 a write failed (disk full); 4 interrupted (Ctrl-C or SIGTERM).
- It runs on the player's PC after the download and is never part of a package.

## What it reads (Azahar's rules)

Taken from `CustomTexManager` (`FindCustomTextures`, `ParseFilename`, `ReadConfig`); the names
are those of `gpu/texture/hash.h` (`CustomTextureStem`), and `tools/native/hd_hash_check.py`
reads a pack by the same rules.

- A file name has at most three dot-separated parts; the last is the extension (`png`, `dds`,
  `ktx`; anything else is ignored), a second-to-last one is a material type (`norm`: a normal
  map of the same texture). A DDS file is ignored when the pack skips mipmaps, which is the case
  without a `pack.json`.
- A file answers to the hash in its name (`tex1_<w>x<h>_<hash>_<format>`, any trailing text such
  as `_mip0` allowed) and to the hashes `pack.json`'s `textures` map gives its file name (the
  directory is not part of the key).
- `pack.json` needs `options.skip_mipmap`, `options.flip_png_files` and `options.use_new_hash`
  (Azahar crashes without them; here they are an error). Without a `pack.json`: the old hash,
  skipped mipmaps, and flipped PNGs (Azahar's member default).
- `flip_png_files`: the PNG rows are turned upside down before use, as Azahar does. DDS data is
  not flipped.
- Two files for the same map of one hash: the first (in path order) is used.
- A hash needs a colour map. A hash is left out of the archive when it has none, when a map
  failed to load, or when its maps differ in size or format (Azahar fails the material then).
- `use_new_hash` is recorded in the archive header: it says which of the two hashes of
  `gpu/texture/hash.h` (`ComputeCustomHashes`) the lookup must use.

## What it writes

For each file: PNG (any colour type) or an uncompressed DDS (RGBA8, BGRA8) is decoded to RGBA8, the
mip chain is built down to 1x1 (a 2x2 mean per level, the last row or column repeated when a side
is odd; colour maps weight the colour by alpha so transparent pixels do not bleed), and every
level is encoded to BC7 (bc7enc_rdo, modes 1 and 6 only). A block-compressed DDS (BC1, BC3, BC5,
BC7) is kept as it is with the levels it has. KTX files are not supported (reported as failed).
Sides up to 16384. Azahar uses only the first level of a DDS; this keeps the others.

Quality (`--quality`): `fast` is mode 6 only; `normal` (the default) adds mode 1 with up to 16
partitions; `high` searches all 64 partitions with the encoder's second effort level.

### Archive layout (little-endian)

```
header  64 bytes: "MH4HDPK1", u32 version (1), u32 flags (bit 0: use_new_hash), u32 entry
        count, u32 entry size (40), u64 index offset, u64 file size, u32 CRC-32 of the index
data    the blobs; a blob is the levels of one image, level 0 first, no padding
index   entry count x 40 bytes at the end, sorted by (hash, map type):
        u64 hash, u64 offset, u64 size, u32 width, u32 height, u32 CRC-32 of the blob,
        u8 map type (0 colour, 1 normal), u8 format (Azahar's CustomPixelFormat:
        0 RGBA8, 1 BC1, 2 BC3, 3 BC5, 4 BC7), u8 mip count, u8 0
```

Level `n` of an entry is `max(1, w >> n) x max(1, h >> n)` pixels, `ceil(w/4) * ceil(h/4)` blocks
of 16 bytes (8 for BC1), row-major. Entries may share a blob (a file mapped to several hashes
is converted once). `ArchiveReader` (`hdpack/archive.h`) opens it, checks the sizes and the index
CRC, and finds an entry by hash; a file whose index was never written is refused.

## Resuming

The data goes to `OUT.part` and each finished file is recorded in `OUT.journal` (the file's path,
size and modification time, where its blob is, its CRC), both flushed per file. A run that finds
them continues if the pack and the settings that change the output (the hash mode, the flip
option, the quality) are the same: it cuts the partial file back to the last complete blob,
ignores a half-written journal record, converts again any file whose size or time changed, and
does the rest. When all files are done the index is written, `OUT.part` becomes `OUT` and the
journal is deleted. `--restart` throws the partial work away.

## Measurements

*(the maintainer, on the reference PC with the real pack: not done yet)*

| | |
|---|---|
| Pack | files, size on disk |
| Time | `mh4n_hdpack PACK -o OUT` with all threads, `normal` quality |
| Archive size | |
| Peak memory | |

## Third-party code and provenance

- `native/third_party/bc7/`: bc7enc_rdo's `bc7enc.cpp` (MIT or public domain, at the user's choice).
- `native/third_party/png/`: LodePNG (zlib licence).
- The pack rules are derived from Azahar; the headers of `pack.*` and `image.*` name the files.
