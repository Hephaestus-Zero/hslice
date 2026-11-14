from pathlib import Path
import shutil
from setuptools import setup
from setuptools.command.build_py import build_py


class BuildPyWithCSources(build_py):
    def run(self) -> None:
        super().run()
        dest = Path(self.build_lib) / "hslice_py" / "_csrc"
        dest.mkdir(parents=True, exist_ok=True)
        for src in Path("src").iterdir():
            if src.suffix in {".c", ".h"} and src.is_file():
                shutil.copy2(src, dest / src.name)


setup(cmdclass={"build_py": BuildPyWithCSources})
