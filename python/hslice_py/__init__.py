"""Zero-overhead native launcher for packaged hslice / hfind / hsearch CLI tools."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys

__version__ = "0.1.0"
_SOURCES = ("hslice_common.h", "hfind.c", "hslice.c")


def _locate_csrc() -> Path:
    pkg_dir = Path(__file__).resolve().parent
    packaged = pkg_dir / "_csrc"
    if (packaged / "hslice.c").is_file():
        return packaged
    repo_src = pkg_dir.parent.parent / "src"
    if (repo_src / "hslice.c").is_file():
        return repo_src
    raise FileNotFoundError("Unable to locate hslice C sources")


def _source_digest(src_dir: Path) -> str:
    h = hashlib.sha256()
    h.update(f"{sys.platform}:{platform.machine()}:".encode())
    for name in _SOURCES:
        h.update((src_dir / name).read_bytes())
    return h.hexdigest()[:16]


def _find_compiler() -> str:
    for cand in (os.environ.get("CC"), "cc", "gcc", "clang"):
        if cand and shutil.which(cand):
            return cand
    raise RuntimeError("No C compiler found on PATH (need cc, gcc, or clang)")


def _ensure_binaries() -> Path:
    src_dir = _locate_csrc()
    digest = _source_digest(src_dir)
    cache_base = Path(
        os.environ.get("HSLICE_CACHE_DIR")
        or os.environ.get("XDG_CACHE_HOME")
        or (Path.home() / ".cache")
    )
    bin_dir = cache_base / "hslice" / f"pybin-{digest}"
    hslice_bin = bin_dir / "hslice"
    hfind_bin = bin_dir / "hfind"
    hsearch_bin = bin_dir / "hsearch"

    if hslice_bin.is_file() and hfind_bin.is_file() and hsearch_bin.exists():
        return bin_dir

    bin_dir.parent.mkdir(parents=True, exist_ok=True)
    tmp_dir = bin_dir.parent / f".tmp-{digest}-{os.getpid()}"
    tmp_dir.mkdir(parents=True, exist_ok=True)
    cc = _find_compiler()
    cflags = ["-std=c11", "-O3", "-Wall", "-Wextra"]
    subprocess.run(
        [cc, *cflags, f"-I{src_dir}", "-o", str(tmp_dir / "hslice"), str(src_dir / "hslice.c")],
        check=True,
    )
    subprocess.run(
        [cc, *cflags, f"-I{src_dir}", "-o", str(tmp_dir / "hfind"), str(src_dir / "hfind.c")],
        check=True,
    )
    (tmp_dir / "hsearch").symlink_to("hfind")
    try:
        tmp_dir.replace(bin_dir)
    except OSError:
        shutil.rmtree(tmp_dir, ignore_errors=True)
    return bin_dir


def _exec_tool(tool_name: str) -> None:
    bin_dir = _ensure_binaries()
    target = str(bin_dir / tool_name)
    os.execv(target, [tool_name, *sys.argv[1:]])


def run_hslice() -> None:
    _exec_tool("hslice")


def run_hfind() -> None:
    _exec_tool("hfind")


def run_hsearch() -> None:
    _exec_tool("hsearch")
