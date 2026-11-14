# Security Policy

## Reporting a Vulnerability

Please report parser panics, out-of-bounds reads on malformed index or source files, or command-argument issues privately through [GitHub Private Vulnerability Reporting](https://github.com/Hephaestus-Zero/hslice/security/advisories/new).

Cache files (`hsym_*.idx` and `hfind_*.idx`) are created with `0700`/`0600` permissions inside `$HSLICE_CACHE_DIR`, `$XDG_RUNTIME_DIR/hslice`, or `/tmp/hslice-<uid>`.
