# Catch2 (vendored)

This directory contains a vendored copy of [Catch2](https://github.com/catchorg/Catch2)'s amalgamated single-translation-unit distribution, used by Jbox's C++ engine tests.

**Version:** v3.7.1
**Source:** https://github.com/catchorg/Catch2/tree/v3.7.1/extras
**License:** Boost Software License 1.0 (see `LICENSE.txt`)

## Files

- `include/catch_amalgamated.hpp` — the amalgamated header.
- `catch_amalgamated.cpp` — the amalgamated implementation. Builds as a regular SPM C++ target; no special configuration needed.
- `LICENSE.txt` — Catch2's BSL-1.0 license text (reproduced for distribution compliance).

## Updating

1. Pick a new Catch2 release from https://github.com/catchorg/Catch2/releases.
2. Re-download both amalgamated files from `https://raw.githubusercontent.com/catchorg/Catch2/<TAG>/extras/`.
3. Also re-download `LICENSE.txt` from the same tag.
4. Update the `**Version:**` line above.
5. Run `swift run JboxEngineCxxTests` to verify nothing broke.
