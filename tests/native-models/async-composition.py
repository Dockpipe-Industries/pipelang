#!/usr/bin/env python3
"""Contained local async composition admission/refusal oracle; never builds."""
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
    result = json.loads(process.stdout)
    assert result["protocol"] == "pipelang.native-model.v1", result
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    verify_job()
    expected = {"memory.max": 1 << 30, "memory.swap.max": 0, "pids.max": 128}
    group = current_group()
    assert {name: int((group / name).read_text()) for name in expected} == expected
    if not args.compiler.is_absolute() or not args.output.is_absolute():
        parser.error("absolute selected compiler and output paths required")
    response = invoke(args.compiler, Path(__file__).with_name("async-composition.pipe").read_text())
    assert response["ok"] is True, response
    child = "private async Task<Result<int,string>> Child() { return Result<int,string>.Success(1); }"
    cases = [
        "public Class M { " + child + " public void Go() { Child(); } }",
        "public Class M { " + child + " public pure int Go() { Result<int,string> r = await Child(); return r.Value; } }",
        "public Class M { " + child + " public int Go() { Result<int,string> r = await Child(); return r.Value; } }",
        "public Class M { " + child + " public async Task<Result<int,string>> Go() { return await Child(); } }",
        "public Class M { " + child + " public async Task<Result<int,string>> Go() { int n = 1 + (await Child()).Value; return Result<int,string>.Success(n); } }",
        "public Class M { " + child + " public Task<Result<int,string>> leaked; }",
        "public Class M { " + child + " public async Task<Result<int,string>> Go() { Task<Result<int,string>> leaked = Child(); return Result<int,string>.Success(1); } }",
        "public Class M { public async Task<Result<int,string>> Go() { Result<int,string> r = await Go(); return r; } }",
        "public Class M { public async Task<Result<int,string>> A() { Result<int,string> r = await B(); return r; } private async Task<Result<int,string>> B() { Result<int,string> r = await A(); return r; } }",
    ]
    # Leaf-first declarations ensure cached call summaries still account for
    # total expanded depth rather than accepting each method in isolation.
    methods = ["private async Task<Result<int,string>> F0() { return Result<int,string>.Success(1); }"]
    methods += [f"private async Task<Result<int,string>> F{i}() {{ Result<int,string> r = await F{i - 1}(); return r; }}"
                for i in range(1, 34)]
    cases.append("public Class M { " + " ".join(methods) + " }")
    for source in cases:
        result = invoke(args.compiler, source)
        assert result["ok"] is False and result["diagnostics"], result
        assert "header" not in result and "manifest" not in result
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "AsyncComposition.hpp").write_text(response["header"])
    (args.output / "AsyncComposition.manifest.json").write_text(json.dumps(response["manifest"], indent=2) + "\n")
    print(json.dumps({"status": "passed", "refusals": len(cases)}))


if __name__ == "__main__":
    main()
