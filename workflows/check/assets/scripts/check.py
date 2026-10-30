#!/usr/bin/env python3
"""Check repository hygiene without compiling generated programs or publishing."""

import ast
import json
import os
from pathlib import Path
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[4]


def run(arguments):
    subprocess.run(arguments, cwd=ROOT, check=True, timeout=90)


def main():
    dockpipe = Path(os.environ["DOCKPIPE_BIN"])
    if not dockpipe.is_absolute() or not dockpipe.is_file():
        raise ValueError("DOCKPIPE_BIN must select an installed absolute executable")
    config = json.loads((ROOT / "dockpipe.config.json").read_text())
    lock = json.loads((ROOT / "dockpipe.lock.json").read_text())
    version = subprocess.check_output([str(dockpipe), "--version"], text=True, timeout=15).strip()
    if version != config["dockpipe_version"] or version != lock["dockpipe"]["version"]:
        raise ValueError("Installed DockPipe, project config and release lock must agree")

    for workflow in sorted((ROOT / "workflows").glob("*/config.yml")):
        run([str(dockpipe), "workflow", "validate", str(workflow)])

    sources = [str(path) for directory in ("src", "sdk", "examples")
               for path in sorted((ROOT / directory).rglob("*.go"))]
    unformatted = subprocess.check_output(["gofmt", "-l", *sources], cwd=ROOT, text=True, timeout=60)
    if unformatted:
        raise ValueError("Go formatting drift:\n" + unformatted)

    for script in sorted((ROOT / "workflows").rglob("*.sh")):
        run(["bash", "-n", str(script)])
    for directory in ("workflows", "tooling", "tests/pipeview"):
        for script in sorted((ROOT / directory).rglob("*.py")):
            ast.parse(script.read_text(), filename=str(script))

    suite = unittest.defaultTestLoader.discover(str(ROOT / "tooling"), pattern="test_*.py")
    if not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful():
        raise RuntimeError("Repository toolchain and delivery contract checks failed")
    print("Repository formatting, workflows, toolchain pins and delivery contracts passed")


if __name__ == "__main__":
    main()
