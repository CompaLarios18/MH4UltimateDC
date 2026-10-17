# LodePNG (vendored)

- Library: LodePNG by Lode Vandevenne, a PNG encoder and decoder in C/C++.
- Upstream: https://github.com/lvandeve/lodepng
- Commit: `ed6fe5825c6a4fbb7f58ab35a4231c7543cd452a` (2026-05-28; the repository has no tags).
  `LODEPNG_VERSION_STRING` in `lodepng.cpp` is `20260119`. A newer commit exists (2026-10-06)
  but it is less than two weeks old.
- Licence: zlib licence (`LICENSE`).
- Used for: decoding the PNG files of the HD texture pack (`native/src/hdpack/image.cpp`,
  N2-17); the encoder half is used by `native/tests/hdpack_test.cpp` to make synthetic PNGs.
- Files are unmodified (SHA-256):

```
d98e1f40d303c1038a096ebf93b413a565a91cf2c72b9d2fa5c625c4279c3cb6  lodepng.cpp
23c27abb06883ed98184d16d0b20771b526dca1e8e13236c2397316769c0dc8b  lodepng.h
bdafa318b8e637d600ca1b60545e894881977a16a16b74809c95e805701621e3  LICENSE
```

Fetched with `git clone https://github.com/lvandeve/lodepng` at the commit above.

Updating: replace the files from the new commit, change the commit and the hashes here, run
`hdpack_test`.
