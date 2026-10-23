#!/usr/bin/env python3
"""Run the Windows portable acceptance slice through a Dockpipe host workflow."""
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[4]
ENVIRONMENT = dict(os.environ, GOWORK="off", GOTOOLCHAIN="local")

subprocess.run(["go", "build", "./..."], cwd=ROOT, env=ENVIRONMENT, check=True)
subprocess.run(["go", "test", "-p", "1", "./src/lib/application", "./sdk/materialize", "./src/cmd/...", "./tests/pipelangcompat"], cwd=ROOT, env=ENVIRONMENT, check=True)
