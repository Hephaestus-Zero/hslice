import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time
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

    def test_05_zero_daemon_lazy_mtime_auto_refresh(self) -> None:
        with tempfile.TemporaryDirectory() as cache_dir, tempfile.TemporaryDirectory() as repo:
            root = Path(repo)
            self._create_sample_repo(root)
            env = dict(os.environ)
            env["HSLICE_CACHE_DIR"] = cache_dir

            c_file = root / "pkg" / "packet.c"
            py_file = root / "pkg" / "router.py"

            # 1. Initial slice populates hsym.idx
            c_init = subprocess.run(
                [str(HSLICE_BIN), "--root", repo, "compute_packet_crc"],
                env=env,
                capture_output=True,
                text=True,
                check=True,
            )
            m_init = re.search(r"=== .+?:(\d+)-(\d+) \[fn compute_packet_crc\] ===", c_init.stdout)
            self.assertIsNotNone(m_init)
            orig_c_start, orig_c_end = int(m_init.group(1)), int(m_init.group(2))

            py_init = subprocess.run(
                [str(HSLICE_BIN), "--root", repo, "route_payment"],
                env=env,
                capture_output=True,
                text=True,
                check=True,
            )
            m_py_init = re.search(r"=== .+?:(\d+)-(\d+) \[fn route_payment\] ===", py_init.stdout)
            self.assertIsNotNone(m_py_init)
            orig_py_start, orig_py_end = int(m_py_init.group(1)), int(m_py_init.group(2))

            # 2. Insert 15 lines above compute_packet_crc in packet.c & 12 lines above OrderRouter in router.py
            c_prefix = "\n".join(f"/* inserted header padding line {i} */" for i in range(15)) + "\n"
            c_file.write_text(c_prefix + C_FIXTURE, encoding="utf-8")

            py_prefix = "\n".join(f"# inserted python padding line {i}" for i in range(12)) + "\n"
            py_extra_fn = "\n\ndef newly_added_validator(token: str) -> bool:\n    return len(token) > 3\n"
            py_file.write_text(py_prefix + PYTHON_FIXTURE + py_extra_fn, encoding="utf-8")

            future_ts = time.time() + 3.0
            os.utime(c_file, (future_ts, future_ts))
            os.utime(py_file, (future_ts, future_ts))

            # 3. Query WITHOUT -r / --rebuild: lazy stat().st_mtime > built_epoch must self-heal
            c_shifted = subprocess.run(
                [str(HSLICE_BIN), "--root", repo, "compute_packet_crc"],
                env=env,
                capture_output=True,
                text=True,
                check=True,
            )
            m_shift = re.search(r"=== .+?:(\d+)-(\d+) \[fn compute_packet_crc\] ===", c_shifted.stdout)
            self.assertIsNotNone(m_shift)
            self.assertEqual(int(m_shift.group(1)), orig_c_start + 15)
            self.assertEqual(int(m_shift.group(2)), orig_c_end + 15)
            self.assertIn("/* Comment with stray braces", c_shifted.stdout)
            self.assertIn("return acc;", c_shifted.stdout)
            self.assertNotIn("helper_caller", c_shifted.stdout)

            py_shifted = subprocess.run(
                [str(HSLICE_BIN), "--root", repo, "route_payment"],
                env=env,
                capture_output=True,
                text=True,
                check=True,
            )
            m_py_shift = re.search(r"=== .+?:(\d+)-(\d+) \[fn route_payment\] ===", py_shifted.stdout)
            self.assertIsNotNone(m_py_shift)
            self.assertEqual(int(m_py_shift.group(1)), orig_py_start + 12)
            self.assertEqual(int(m_py_shift.group(2)), orig_py_end + 12)
            self.assertIn("@staticmethod", py_shifted.stdout)
            self.assertIn("raise ValueError", py_shifted.stdout)
            self.assertNotIn("dispatch_worker", py_shifted.stdout)

            # Brand-new function in the modified file is also indexed now without -r
            new_fn_slice = subprocess.run(
                [str(HSLICE_BIN), "--root", repo, "newly_added_validator"],
                env=env,
                capture_output=True,
                text=True,
                check=True,
            )
            self.assertIn("def newly_added_validator", new_fn_slice.stdout)
            self.assertIn("return len(token) > 3", new_fn_slice.stdout)

    def test_06_audit_receipt_metrics_and_flags(self) -> None:
        with tempfile.TemporaryDirectory() as cache_dir, tempfile.TemporaryDirectory() as repo:
            root = Path(repo)
            self._create_sample_repo(root)
            env = dict(os.environ)
            env["HSLICE_CACHE_DIR"] = cache_dir

            # Audit fixture repository via --audit <dir>
            audit_fixture = subprocess.run(
                [str(HSLICE_BIN), "--audit", repo],
                env=env,
                capture_output=True,
                text=True,
                check=True,
            )
            out = audit_fixture.stdout
            self.assertIn("hslice audit", out)
            self.assertIn("github.com/Hephaestus-Zero/hslice", out)
            self.assertIn("─", out)
            self.assertNotIn("\u2014", out)
            self.assertNotIn("\u2013", out)

            m_all = re.search(r"all ([\d,]+) indexed symbols?\s+([\d,]+)\s+([\d,]+)\s+([\d.]+)%", out)
            self.assertIsNotNone(m_all, f"Missing aggregate line in:\n{out}")
            n_syms = int(m_all.group(1).replace(",", ""))
            file_tok = int(m_all.group(2).replace(",", ""))
            slice_tok = int(m_all.group(3).replace(",", ""))
            saved_pct = float(m_all.group(4))

            self.assertGreaterEqual(n_syms, 8)
            self.assertGreater(file_tok, slice_tok)
            self.assertGreater(slice_tok, 0)
            expected_pct = 100.0 * (file_tok - slice_tok) / file_tok
            self.assertAlmostEqual(saved_pct, expected_pct, delta=0.15)

            m_footer = re.search(r"context saved: ([\d.]+)%  ·  density: ([\d.]+)x  ·  elapsed: ([\d.]+) ms", out)
            self.assertIsNotNone(m_footer, f"Missing summary metrics in:\n{out}")
            self.assertAlmostEqual(float(m_footer.group(1)), saved_pct, places=1)
            self.assertGreater(float(m_footer.group(2)), 1.5)

            # Audit hslice repository itself via -A
            audit_self = subprocess.run(
                [str(HSLICE_BIN), "-A", str(REPO_ROOT)],
                env=env,
                capture_output=True,
                text=True,
                check=True,
            )
            self.assertIn("hslice audit  hslice", audit_self.stdout)
            self.assertIn("context saved:", audit_self.stdout)

            # Nonexistent directory returns non-zero cleanly
            bad_dir = subprocess.run(
                [str(HSLICE_BIN), "--audit", str(root / "does_not_exist")],
                env=env,
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(bad_dir.returncode, 0)


if __name__ == "__main__":
    unittest.main()
