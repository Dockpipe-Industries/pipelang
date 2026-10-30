#!/usr/bin/env python3
"""Compile public widget-handle consumers; reject payload/arity weakening."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
IMPORT = "github.com/Dockpipe-Industries/pipelang/sdk/pipeview/testing"


def main():
    cases = [
        ("typed", '_ = toggle.Change(true); _ = field.Change("Rider"); _ = stepper.Change(int64(100)); _, _ = button.Invoke()', True),
        ("toggle-text", '_ = toggle.Change("false")', False),
        ("field-boolean", '_ = field.Change(true)', False),
        ("button-payload", '_, _ = button.Invoke(true)', False),
        ("stepper-text", '_ = stepper.Change("100")', False),
        ("stepper-boolean", '_ = stepper.Change(true)', False),
        ("stepper-float", '_ = stepper.Change(1.5)', False),
    ]
    environment = dict(os.environ, GOWORK="off")
    with tempfile.TemporaryDirectory(prefix="pipeview-handles-") as directory:
        target = Path(directory)
        for name, body, accepted in cases:
            source = target / (name + ".go")
            source.write_text('package main\nimport viewtest "' + IMPORT + '"\n'
                              'func exercise(toggle viewtest.Toggle, field viewtest.TextField, button viewtest.Button, stepper viewtest.PercentStepper) {\n'
                              + body + '\n}\nfunc main() {}\n')
            result = subprocess.run(["go", "build", "-p=1", "-o", str(target / name), str(source)],
                                    cwd=ROOT, env=environment, text=True, capture_output=True, timeout=60)
            if accepted and result.returncode:
                raise RuntimeError("Typed handle consumer failed: " + result.stderr)
            if not accepted and (result.returncode == 0 or not any(
                    marker in result.stderr for marker in ("cannot use", "too many arguments", "truncated"))):
                raise RuntimeError("Handle type refusal missing: " + name + " " + result.stderr)
    print("PipeView testing: typed consumer and six invalid handle calls verified")


if __name__ == "__main__":
    main()
