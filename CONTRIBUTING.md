# Contributing

Thanks for helping! Read [`docs/LEGAL.md`](docs/LEGAL.md) first — the one non-negotiable rule is **no copyrighted game data in the repo**.

## Useful contributions right now
- Verified dump info (title ID, version, SHA-1 of `code.bin`/exheader) for your region → `config/versions.json`
- Function names, struct layouts and file-format notes from your own reverse engineering → `docs/research/`
- Tooling improvements (with tests)

## Conventions
- Python tools: 3.10+, standard library preferred, tests in `tests/` (`python3 -m pytest`)
- Addresses are written as `0x00123456` (8 hex digits, lowercase `0x`)
- Commit messages: imperative mood, short subject line (`Add exheader parser`, not `added stuff`)
- Discussions in English or Spanish are both fine

## Before opening a PR
- `python3 -m pytest` passes
- `git status` shows no files from `baserom/`, `build/`, or any game data
