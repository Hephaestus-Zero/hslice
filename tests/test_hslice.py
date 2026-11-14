import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

REPO_ROOT = Path(__file__).resolve().parent.parent
HSLICE_BIN = REPO_ROOT / "hslice"
HFIND_BIN = REPO_ROOT / "hfind"
HSEARCH_BIN = REPO_ROOT / "hsearch"

PYTHON_FIXTURE = '''"""Sample service module."""

def decoy_before():
    msg = """
def fake_inside_docstring():
    return 0
"""
    return msg


class OrderRouter:
    """Routes validated payments."""

    @staticmethod
    async def route_payment(amount_cents: int, currency: str = "USD") -> str:
        """Normalize and route an order."""
        if amount_cents <= 0:
            raise ValueError("negative amount")
        return f"{currency}:{amount_cents}"


def dispatch_worker(order_id: str) -> str:
    return OrderRouter.route_payment(1200, order_id)
'''

C_FIXTURE = '''#include <stdio.h>

struct SliceResult {
    int status_code;
    const char *label;
};

/* Comment with stray braces { } should not close function */
int compute_packet_crc(const char *buf, int len) {
    const char *brace_str = "} not end of fn {";
    int acc = brace_str[0] == '}' ? 1 : 0;
    for (int i = 0; i < len; i++) {
        acc = (acc * 33) ^ (unsigned char)buf[i];
    }
    return acc;
}

void helper_caller(void) {
    (void)compute_packet_crc("abc", 3);
}
'''

GO_FIXTURE = '''package main

type EngineConfig struct {
    Workers int
}

func StartWorkers(cfg EngineConfig) int {
    return cfg.Workers * 2
}
'''

TS_FIXTURE = '''export interface JobSpec {
    id: string;
}

export async function runPipeline(spec: JobSpec): Promise<string> {
    return spec.id;
}
'''


class TestHSliceEngine(unittest.TestCase):
    def _create_sample_repo(self, root: Path, tag: str = "alpha") -> None:
        (root / ".git").mkdir()
        src = root / "pkg"
        src.mkdir()
        (src / "router.py").write_text(PYTHON_FIXTURE, encoding="utf-8")
        (src / "packet.c").write_text(C_FIXTURE, encoding="utf-8")
        (src / "worker.go").write_text(GO_FIXTURE, encoding="utf-8")
        (src / "pipeline.ts").write_text(TS_FIXTURE, encoding="utf-8")
        (root / f"notes_{tag}.md").write_text(f"# Workspace {tag}\n", encoding="utf-8")

    def test_01_hfind_hsearch_and_workspace_isolation(self) -> None:
        with tempfile.TemporaryDirectory() as cache_dir, \
             tempfile.TemporaryDirectory() as repo_a, \
             tempfile.TemporaryDirectory() as repo_b:
            pa, pb = Path(repo_a), Path(repo_b)
            self._create_sample_repo(pa, tag="alpha")
            (pb / ".git").mkdir()
            (pb / "only_in_beta.py").write_text("def beta_only_fn():\n    return 42\n", encoding="utf-8")

            env = dict(os.environ)
            env["HSLICE_CACHE_DIR"] = cache_dir

            # Run inside repo_a subdirectory to prove upward .git discovery works
            sub_a = pa / "pkg"
            f_res = subprocess.run(
                [str(HFIND_BIN), "--stats", "router.py"],
                cwd=sub_a,
                env=env,
                capture_output=True,
                text=True,
                check=True,
            )
            self.assertIn("router.py", f_res.stdout)
            self.assertNotIn("only_in_beta.py", f_res.stdout)

            # Querying repo_b gets a separate hashed index and must not leak repo_a symbols
            s_b = subprocess.run(
                [str(HSEARCH_BIN), "beta_only_fn"],
                cwd=pb,
                env=env,
                capture_output=True,
                text=True,
                check=True,
            )
            self.assertIn("beta_only_fn", s_b.stdout)
            s_a_in_b = subprocess.run(
                [str(HSEARCH_BIN), "compute_packet_crc"],
                cwd=pb,
                env=env,
                capture_output=True,
                text=True,
            )
            self.assertNotIn("compute_packet_crc", s_a_in_b.stdout)

            # Language coverage in repo_a (Python, C, Go, TS)
            for sym in ("route_payment", "compute_packet_crc", "StartWorkers", "runPipeline"):
                r = subprocess.run(
                    [str(HSEARCH_BIN), "--root", repo_a, sym],
                    env=env,
                    capture_output=True,
                    text=True,
                    check=True,
                )
                self.assertIn(sym, r.stdout)

    def test_02_hslice_exact_symbol_bounds_python_and_c(self) -> None:
        with tempfile.TemporaryDirectory() as cache_dir, tempfile.TemporaryDirectory() as repo:
            root = Path(repo)
            self._create_sample_repo(root)
            env = dict(os.environ)
            env["HSLICE_CACHE_DIR"] = cache_dir

            # Python async decorated method: must include @staticmethod and full body, and stop before dispatch_worker
            py_slice = subprocess.run(
                [str(HSLICE_BIN), "--root", repo, "route_payment"],
                env=env,
                capture_output=True,
                text=True,
                check=True,
            )
            self.assertIn("@staticmethod", py_slice.stdout)
            self.assertIn("raise ValueError", py_slice.stdout)
            self.assertNotIn("dispatch_worker", py_slice.stdout)

            # C function with braces in comment + string literal: must include full body and stop before helper_caller
            c_slice = subprocess.run(
                [str(HSLICE_BIN), "--root", repo, "compute_packet_crc"],
                env=env,
                capture_output=True,
                text=True,
                check=True,
            )
            self.assertIn("/* Comment with stray braces", c_slice.stdout)
            self.assertIn("return acc;", c_slice.stdout)
            self.assertNotIn("helper_caller", c_slice.stdout)

    def test_03_traceback_enclosing_range_and_skeleton(self) -> None:
        with tempfile.TemporaryDirectory() as cache_dir, tempfile.TemporaryDirectory() as repo:
            root = Path(repo)
            self._create_sample_repo(root)
            env = dict(os.environ)
            env["HSLICE_CACHE_DIR"] = cache_dir

            py_file = root / "pkg" / "router.py"
            lines = py_file.read_text(encoding="utf-8").splitlines()
            target_line = next(i + 1 for i, ln in enumerate(lines) if "raise ValueError" in ln)

            tb_slice = subprocess.run(
                [str(HSLICE_BIN), "--root", repo, f"{py_file}:{target_line}"],
                env=env,
                capture_output=True,
                text=True,
                check=True,
            )
            self.assertIn(f"@L{target_line}", tb_slice.stdout)
            self.assertIn("async def route_payment", tb_slice.stdout)
            self.assertNotIn("decoy_before", tb_slice.stdout)

            # Explicit range slice
            range_slice = subprocess.run(
                [str(HSLICE_BIN), "--root", repo, f"{py_file}:1-4"],
                env=env,
                capture_output=True,
                text=True,
                check=True,
            )
            self.assertIn("1: \"\"\"Sample service module.\"\"\"", range_slice.stdout)

            # Skeleton mode
            c_file = root / "pkg" / "packet.c"
            skel = subprocess.run(
                [str(HSLICE_BIN), "--root", repo, "-s", str(c_file)],
                env=env,
                capture_output=True,
                text=True,
                check=True,
            )
            self.assertIn("SKELETON", skel.stdout)
            self.assertIn("compute_packet_crc", skel.stdout)
            self.assertIn("SliceResult", skel.stdout)

    def test_04_xref_one_shot_and_grep_fallback(self) -> None:
        with tempfile.TemporaryDirectory() as cache_dir, \
             tempfile.TemporaryDirectory() as repo, \
             tempfile.TemporaryDirectory() as fake_bin:
            root = Path(repo)
            self._create_sample_repo(root)
            env = dict(os.environ)
            env["HSLICE_CACHE_DIR"] = cache_dir

            # 1-shot xref in normal environment
            xr = subprocess.run(
                [str(HSLICE_BIN), "--root", repo, "-x", "compute_packet_crc"],
                env=env,
                capture_output=True,
                text=True,
                check=True,
            )
            self.assertIn("int acc =", xr.stdout)
            self.assertIn("REFS (compute_packet_crc", xr.stdout)
            self.assertIn("(void)compute_packet_crc", xr.stdout)

            # Test POSIX grep fallback when rg is absent from PATH
            grep_exe = shutil.which("grep")
            self.assertIsNotNone(grep_exe)
            (Path(fake_bin) / "grep").symlink_to(grep_exe)
            env_no_rg = dict(env)
            env_no_rg["PATH"] = fake_bin

            xr_grep = subprocess.run(
                [str(HSLICE_BIN), "--root", repo, "-x", "compute_packet_crc"],
                env=env_no_rg,
                capture_output=True,
                text=True,
                check=True,
            )
            self.assertIn("REFS (compute_packet_crc", xr_grep.stdout)
            self.assertIn("(void)compute_packet_crc", xr_grep.stdout)


if __name__ == "__main__":
    unittest.main()
