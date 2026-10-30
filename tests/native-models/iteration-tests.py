#!/usr/bin/env python3
"""Generate iteration oracle inside containedexec; parent compiles separately."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "containedexec"))
from job import current_group, verify_job


def invoke(compiler, source):
    request = {"protocol": "pipelang.native-model.v1", "source": source,
               "namespace": "pipelang_models"}
    process = subprocess.run([str(compiler)], input=json.dumps(request), text=True,
                             capture_output=True, timeout=10, check=False)
    assert process.returncode == 0, process.stderr
    assert len(process.stdout) <= 4 << 20 and len(process.stderr) <= 65536
    return json.loads(process.stdout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    verify_job()
    group = current_group()
    expected = {"memory.max": 1 << 30, "memory.swap.max": 0, "pids.max": 128}
    assert {key: int((group / key).read_text()) for key in expected} == expected
    if not args.compiler.is_absolute() or not args.output.is_absolute():
        parser.error("absolute paths required")
    result = invoke(args.compiler, Path(__file__).with_name("iteration-tests.pipe").read_text())
    assert result["ok"] is True, result
    cases = [
        "public Class M { public void Go() { break; } }",
        "public Class M { public void Go() { continue; } }",
        "public Class M { public void Go() { while (1) {} } }",
        "public Class M { public void Go() { foreach (int item in 1) {} } }",
        "public Class M { public void Go() { List<int> values = List<int>.Empty(); foreach (string item in values) {} } }",
        "public Class M { public void Go() { List<int> values = List<int>.Empty(); foreach (int item in values) {} int copy = item; } }",
        "public Class M { public void Go() { List<int> values = List<int>.Empty(); int item = 0; foreach (int item in values) {} } }",
        "public Class M { public List<void> values; }",
        "public Class M { public List<CancellationToken> values; }",
        "public Class M { public void Go() { List<int> values = List<int>.Empty(); values.Add(1.0); } }",
        "public Class M { public pure int Go() { List<int> values = List<int>.Empty(); return values.At(0.0); } }",
        "public Class M { public List<int> values; public pure int Go() { values.Add(1); return 0; } }",
    ]
    for source in cases:
        refusal = invoke(args.compiler, source)
        assert refusal["ok"] is False and refusal["diagnostics"], refusal
        assert "header" not in refusal and "manifest" not in refusal
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "Iteration.hpp").write_text(result["header"])
    (args.output / "Iteration.manifest.json").write_text(json.dumps(result["manifest"], indent=2) + "\n")
    print(json.dumps({"status": "passed", "refusals": len(cases)}))


if __name__ == "__main__":
    main()
