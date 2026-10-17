# xxHash (vendored)

- Library: xxHash by Yann Collet, https://github.com/Cyan4973/xxHash
- Version: 0.8.3 (tag `v0.8.3`, `XXH_VERSION_MAJOR.MINOR.RELEASE` = 0.8.3 in `xxhash.h`)
- Licence: BSD 2-Clause for the library (`xxhash.h`, `LICENSE`); the command-line tool, which is
  not vendored, is GPL-2.0-or-later.
- Files: `xxhash.h` (the single header, used with `XXH_INLINE_ALL`) and `LICENSE`, both
  unmodified from the tag.
- Used for: the content hash of the texture cache (`native/src/gpu/texture/cache.cpp`, XXH3 64-bit).
  It is not used for the custom-texture hashes: those are CityHash64, as Azahar's.
- Fetched with `curl https://raw.githubusercontent.com/Cyan4973/xxHash/v0.8.3/xxhash.h` (and
  `LICENSE`); the tag is more than a year old.

Updating: replace the two files from the new tag, change the version here, build, run
`gpu_texture_test`.
