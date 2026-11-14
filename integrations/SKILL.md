---
name: hslice
description: Sub-2ms AST/structural code slicer and trigram search engine (hslice, hfind, hsearch). Use instead of multi-step find/grep/read loops to inspect exact functions, traceback lines, file outlines, and callers in a single call with ~88% fewer context tokens.
---

# `hslice`: Single-Shot Structural Code Slicer

Replace multi-turn `find -> grep -> read entire file -> grep callers` workflows with one call:

1. **Extract a function, class, or struct by symbol name** (1-shot body + callers):
   ```bash
   hslice -x <symbol>
   ```
2. **Jump straight to an error traceback line's enclosing function**:
   ```bash
   hslice <path/to/file.py>:<line>
   ```
3. **Read a file's compact structural outline (~90% fewer tokens)**:
   ```bash
   hslice -s <path/to/file>
   ```
4. **Locate files or symbol definitions in <2 ms**:
   ```bash
   hfind <path tokens...>
   hsearch <symbol tokens...>
   hsearch -g "<regex>" [dir]
   ```

## Indexing Scope
- Automatically scopes its mmap trigram index to the enclosing Git repository root (or `$PWD`), stored per-workspace under `$HSLICE_CACHE_DIR` / `$XDG_RUNTIME_DIR/hslice` / `/tmp/hslice-<uid>`.
- Pass `--root <dir1[:dir2]>` or set `HSLICE_ROOTS` to index custom trees, or `--rebuild` after heavy edits.
