#!/usr/bin/env python3
"""Check PipeView package isolation and public Go type-safety contracts."""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile


MODULE = "github.com/Dockpipe-Industries/pipelang"
ROOT = Path(__file__).resolve().parents[2]


def run(arguments, **options):
    return subprocess.run(arguments, cwd=ROOT, text=True, capture_output=True,
                          timeout=60, **options)


def dependencies(package):
    result = run(["go", "list", "-deps", "-f", "{{if not .Standard}}{{.ImportPath}}{{end}}", package], check=True)
    return set(result.stdout.split())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dependencies-only", action="store_true")
    arguments = parser.parse_args()
    core = MODULE + "/sdk/pipeview"
    adapter = core + "/forgepipe"
    if dependencies("./sdk/pipeview") != {core}:
        raise RuntimeError("PipeView core acquired a non-standard dependency")
    if dependencies("./sdk/pipeview/forgepipe") != {core, adapter}:
        raise RuntimeError("ForgePipe transport acquired an engine/compiler/backend dependency")
    if any(item.startswith(core) for item in dependencies("./sdk/materialize")):
        raise RuntimeError("Headless materializer acquired a UI dependency")

    compiler = dependencies("./src/lib/pipelang")
    if any(item.startswith(core) for item in compiler):
        raise RuntimeError("The compiler acquired an optional UI package dependency")
    runtime = core + "/state"
    if dependencies("./sdk/pipeview/state") != {core, runtime}:
        raise RuntimeError("State runtime acquired compiler, renderer or infrastructure dependencies")
    frontend = core + "/source"
    if dependencies("./sdk/pipeview/source") != compiler | {core, frontend, runtime}:
        raise RuntimeError("View source checking acquired a renderer, CLI or unexpected dependency")

    markup = core + "/markup"
    if dependencies("./sdk/pipeview/markup") != compiler | {core, frontend, runtime, markup}:
        raise RuntimeError("XML frontend acquired renderer, CLI or unexpected dependencies")

    inspector = core + "/inspect"
    test_host = core + "/testing"
    if dependencies("./sdk/pipeview/inspect") != {core, runtime, inspector}:
        raise RuntimeError("Inspection acquired a compiler, renderer or I/O dependency")
    if dependencies("./sdk/pipeview/testing") != {core, runtime, inspector, test_host}:
        raise RuntimeError("Test host acquired a compiler, renderer or unexpected dependency")
    if arguments.dependencies_only:
        print("PipeView: core/state/source/markup/inspection/testing dependency boundaries passed")
        return

    cases = [
        ("core-consumer", '''b := pv.State("enabled", true)
            _, err := pv.Compile(pv.Toggle("toggle", "Enabled", b, pv.Change[bool]("edit")))
            if err != nil { panic(err) }''', True),
        ("wrong-binding", '''pv.Toggle("toggle", "Enabled", pv.State("name", "Rider"), pv.Change[bool]("edit"))''', False),
        ("wrong-event", '''pv.Toggle("toggle", "Enabled", pv.State("enabled", true), pv.Change[string]("edit"))''', False),
        ("unsupported-scalar", '''pv.State("volume", float64(0.5))''', False),
        ("percent-consumer", 'pv.PercentStepper("percent", "Volume", pv.State("volume", int64(100)), pv.Change[int64]("edit"))', True),
        ("percent-binding", 'pv.PercentStepper("percent", "Volume", pv.State("volume", "100"), pv.Change[int64]("edit"))', False),
        ("percent-event", 'pv.PercentStepper("percent", "Volume", pv.State("volume", int64(100)), pv.Change[string]("edit"))', False),
    ]
    environment = dict(os.environ, GOWORK="off")
    with tempfile.TemporaryDirectory(prefix="pipeview-package-check-") as directory:
        temporary = Path(directory)
        for name, body, accepted in cases:
            source = temporary / (name + ".go")
            source.write_text('package main\nimport pv "' + core + '"\nfunc main() {\n' + body + '\n}\n')
            result = run(["go", "build", "-p=1", "-o", str(temporary / name), str(source)], env=environment)
            if accepted and result.returncode:
                raise RuntimeError(name + ": " + result.stderr)
            if not accepted and (result.returncode == 0 or not any(
                    marker in result.stderr for marker in ("cannot use", "does not satisfy"))):
                raise RuntimeError(name + " did not produce the expected type refusal: " + result.stderr)
    print("PipeView: core/adapter/state/source/compiler/headless dependency isolation and five public type refusals passed")


if __name__ == "__main__":
    main()
