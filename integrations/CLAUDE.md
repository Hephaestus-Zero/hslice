## Fast Code Recon (`hslice` / `hfind` / `hsearch`)

Prefer sub-2ms `hslice` primitives over reading entire source files into context:
- Unknown file path? Run `hfind <tokens>`
- Looking for a symbol + callers in one shot? Run `hslice -x <symbol>`
- Inspecting a traceback line? Run `hslice <file>:<line>`
- Need a module overview before editing? Run `hslice -s <file>`
- Full-text regex across files? Run `hsearch -g "<regex>" [dir]`
