#!/usr/bin/env python3
"""Reproducible latency & token-economy benchmark for hslice vs traditional multi-turn recon."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import statistics
import subprocess
import tempfile
import time


def run_cmd(cmd: list[str], cwd: Path, env: dict[str, str]) -> tuple[float, int]:
    t0 = time.perf_counter_ns()
    res = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True, check=False)
    dt_ms = (time.perf_counter_ns() - t0) / 1e6
    return dt_ms, len(res.stdout.encode("utf-8"))


def median_bytes_and_ms(fn, rounds: int = 25) -> tuple[float, int]:
    times: list[float] = []
    out_bytes = 0
    for _ in range(rounds):
        dt_ms, nbytes = fn()
        times.append(dt_ms)
        out_bytes = nbytes
    return statistics.median(times), out_bytes


def main() -> None:
    parser = argparse.ArgumentParser(description="Benchmark hslice 1-shot recon vs multi-step find+grep+read.")
    parser.add_argument("--repo", type=Path, required=True, help="Target repository directory")
    parser.add_argument("--symbol", required=True, help="Target symbol name")
    parser.add_argument("--file-hint", required=True, help="Target basename token for find")
    parser.add_argument("--target-file", type=Path, required=True, help="Relative path inside repo to read")
    parser.add_argument("--bin-dir", type=Path, default=Path(__file__).resolve().parent.parent)
    args = parser.parse_args()

    repo = args.repo.resolve()
    full_file = repo / args.target_file
    hslice = str(args.bin_dir / "hslice")
    hfind = str(args.bin_dir / "hfind")
    hsearch = str(args.bin_dir / "hsearch")

    with tempfile.TemporaryDirectory() as cache_dir:
        env = dict(os.environ)
        env["HSLICE_CACHE_DIR"] = cache_dir

        subprocess.run([hfind, "--root", str(repo), "--rebuild"], env=env, capture_output=True, check=True)
        subprocess.run([hsearch, "--root", str(repo), "--rebuild"], env=env, capture_output=True, check=True)

        find_ms, _ = median_bytes_and_ms(lambda: run_cmd([hfind, "--root", str(repo), args.file_hint], repo, env))
        search_ms, _ = median_bytes_and_ms(lambda: run_cmd([hsearch, "--root", str(repo), args.symbol], repo, env))
        def_ms, def_bytes = median_bytes_and_ms(lambda: run_cmd([hslice, "--root", str(repo), args.symbol], repo, env))

        def baseline() -> tuple[float, int]:
            t1, b1 = run_cmd(["find", str(repo), "-maxdepth", "6", "-name", args.file_hint], repo, env)
            t2, b2 = run_cmd(["grep", "-rn", args.symbol, str(repo)], repo, env)
            t3, b3 = run_cmd(["cat", str(full_file)], repo, env)
            t4, b4 = run_cmd(["grep", "-rnw", args.symbol, str(repo)], repo, env)
            return t1 + t2 + t3 + t4, b1 + b2 + b3 + b4

        def one_shot() -> tuple[float, int]:
            return run_cmd([hslice, "--root", str(repo), "-x", args.symbol], repo, env)

        def skeleton() -> tuple[float, int]:
            return run_cmd([hslice, "--root", str(repo), "-s", str(full_file)], repo, env)

        cat_bytes = full_file.stat().st_size
        base_ms, base_bytes = median_bytes_and_ms(baseline)
        shot_ms, shot_bytes = median_bytes_and_ms(one_shot)
        skel_ms, skel_bytes = median_bytes_and_ms(skeleton)

        print(f"Repository: {repo.name} | Symbol: {args.symbol} | File: {args.target_file} ({cat_bytes} B)")
        print(f"  hfind lookup:   {find_ms:.2f} ms")
        print(f"  hsearch lookup: {search_ms:.2f} ms")
        print(f"  hslice symbol:  {def_ms:.2f} ms, {def_bytes} B (~{max(1, round(def_bytes / 4))} tokens)")
        print(f"  Baseline 4-step (find+grep+cat+grep): {base_ms:.2f} ms, {base_bytes} B (~{max(1, round(base_bytes / 4))} tokens)")
        print(f"  hslice -x (1-shot def+refs):         {shot_ms:.2f} ms, {shot_bytes} B (~{max(1, round(shot_bytes / 4))} tokens) "
              f"[{-100.0 * (1.0 - shot_bytes / max(1, base_bytes)):.1f}% tokens, "
              f"{base_ms / max(0.01, shot_ms):.1f}x faster]")
        print(f"  hslice -s vs cat:                    {skel_ms:.2f} ms, {skel_bytes} B (~{max(1, round(skel_bytes / 4))} tokens) "
              f"[{-100.0 * (1.0 - skel_bytes / max(1, cat_bytes)):.1f}% tokens]")


if __name__ == "__main__":
    main()
