# bc7enc (vendored)

- Library: bc7enc_rdo by Richard Geldreich, Jr. (Binomial LLC); only the plain BC7 block
  encoder `bc7enc.cpp` / `bc7enc.h` is vendored, not the RDO parts, the ISPC encoder or the
  other files of the repository.
- Upstream: https://github.com/richgel999/bc7enc_rdo
- Commit: `b9438627eef73a1157e84201b6fa6eb2ffd6d9f0` (2026-07-30, the head of `master` when
  fetched; the repository has no tags). The vendored files last changed in 2021 and are
  byte-identical to that commit.
- Licence: dual, "MIT License or Public Domain (unlicense)", at the choice of the user; this
  project uses it under the MIT alternative. `LICENSE` is the repository's file, which also says
  that `bc7e.ispc` is Apache-2.0 and that LodePNG has its own licence: neither is vendored here.
- Used for: encoding the HD texture pack to BC7 (`native/src/hdpack/bc7.cpp`, N2-17).
- Files are unmodified (SHA-256):

```
b90444a091530a13c61fa6375583fcc9bc9ca47e474fd71f818876de68c1b581  bc7enc.cpp
f40f4a956ed6a8845b7909e83831de09512424408b32ca29551bf49aee2fecf6  bc7enc.h
b3e843763e8d3bdeb0f469ecd4107fc32b5fa27ebee1af10d2066bc339292800  LICENSE
```

Fetched with `git clone https://github.com/richgel999/bc7enc_rdo` and a copy of the three files.
`bc7enc.h` has no include guard: include it from one translation unit only (`bc7.cpp`).

Updating: replace the files from the new commit, change the commit and the hashes here, run
`hdpack_test`.
