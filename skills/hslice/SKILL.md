---
name: hslice
description: Sub-2ms C11 mmap trigram code indexer and structural AST symbol slicer (hslice, hfind, hsearch) that cuts coding agent file-read tokens by ~94%. Use whenever locating files, searching symbols, reading function/class definitions, inspecting stack traces, or tracing callers instead of grep/find/cat/sed.
---

# `hslice` — Single-Shot Structural Code Slicer

Replace multi-turn `find -> grep -> read entire file -> grep callers` loops with one deterministic sub-2ms command:

## Quick Commands

1. **Single-shot definition + all bounded callers/references (`-96%` tokens)**:
   ```bash
   hslice -x <symbol> [dir]
   ```
2. **Exact function, class, or struct definition by symbol name (~1.3 ms)**:
   ```bash
   hslice <symbol>
   ```
3. **Traceback / error line zoom (returns ONLY the enclosing function/class)**:
   ```bash
   hslice <path/to/file>:<line>
   ```
4. **Compact structural file outline (`-93%` tokens vs full file read)**:
   ```bash
   hslice -s <path/to/file>
   ```
5. **Zero-copy `mmap` trigram path, symbol, and regex lookups**:
   ```bash
   hfind <path tokens...>
   hsearch <symbol tokens...>
   hsearch -g "<regex>" [dir]
   ```

## Installation

- **Homebrew (macOS Apple Silicon & Linux):** `brew install Hephaestus-Zero/tap/hslice`
- **Quick Binary / Source Script:** `curl -fsSL https://raw.githubusercontent.com/Hephaestus-Zero/hslice/main/install.sh | sh`
- **Python / `uvx`:** `pip install git+https://github.com/Hephaestus-Zero/hslice.git`

## Workspace Scoping

- Automatically scopes its `262,144`-bucket `mmap` trigram index to the enclosing Git root (or `$PWD`), isolating caches by workspace hash under `$HSLICE_CACHE_DIR`, `$XDG_RUNTIME_DIR/hslice`, or `/tmp/hslice-<uid>`.
- Pass `--root <dir1[:dir2]>` or `HSLICE_ROOTS` to index external trees, or `--rebuild` after bulk edits.
- Supports Python (decorators + indentation + triple-quoted docstrings), C, C++, Rust, Go, JavaScript, TypeScript, and Java.
