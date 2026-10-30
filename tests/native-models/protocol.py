#!/usr/bin/env python3
"""Native model refusal oracle. Run only inside containedexec job.py + run.py.

The compiler path is selected explicitly. This test never compiles source with a
Python parser: every source string is submitted to the selected native frontend.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "containedexec"))
from job import current_group, verify_job


def verify_containment():
    verify_job()
    group = current_group()
    expected = {"memory.max": 1 << 30, "memory.swap.max": 0, "pids.max": 128}
    actual = {name: int((group / name).read_text()) for name in expected}
    if actual != expected:
        raise RuntimeError("native model protocol tests require a containedexec unit")


def invoke(compiler, source=None, raw=None, **request_fields):
    request = {"protocol": "pipelang.native-model.v1", "source": source,
               "namespace": "test_models", **request_fields}
    process = subprocess.run([str(compiler)], input=raw if raw is not None else json.dumps(request),
                             text=True, capture_output=True, timeout=10, check=False)
    if len(process.stdout) > 4 << 20 or len(process.stderr) > 65536:
        raise AssertionError("compiler response budget exceeded")
    return process


def admitted(compiler, source):
    process = invoke(compiler, source)
    assert process.returncode == 0, process.stderr
    result = json.loads(process.stdout)
    assert result["ok"] is True, result
    assert set(result) == {"protocol", "ok", "header", "manifest"}
    assert result["protocol"] == "pipelang.native-model.v1"
    assert result["manifest"]["protocol"] == "pipelang.native-model-manifest.v1"
    return result


def refused(compiler, source, message):
    process = invoke(compiler, source)
    assert process.returncode == 0, process.stderr
    result = json.loads(process.stdout)
    assert result["ok"] is False, result
    assert "header" not in result and "manifest" not in result
    assert message in result["diagnostics"][0]["message"], result
    assert isinstance(result["diagnostics"][0]["offset"], int)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True, type=Path)
    args = parser.parse_args()
    verify_containment()
    if not args.compiler.is_absolute():
        parser.error("absolute selected compiler path required")
    program = (Path(__file__).with_name("Models.pipe")).read_text()
    response = admitted(args.compiler, program)
    model = response["manifest"]["classes"][0]
    assert model["name"] == "Model"
    assert all(field["name"] not in {"Service", "secret"} for field in model["fields"])
    assert model["constructor"]["parameters"] == [
        {"name": "service", "type": "Processor"}, {"name": "initial", "type": "Payload"}]
    admitted(args.compiler, "public Class Empty { public int count = 0; }")
    refusal_cases = [
        ("public Class Model { public int count; public pure int Read() { count = 2; return count; } }",
         "pure methods cannot mutate"),
        ("public Class Model { public void Edit() {} public pure int Read() { int value = Edit(); return value; } }",
         "pure methods cannot call mutating helpers"),
        ("public Class Model { public pure int Read() => Read(); }", "recursive local call"),
        ("public Record Broken { public Broken value; }", "recursive record"),
        ("public Class Model { public int value; public pure int value() => 1; }", "duplicate method"),
        ("public Class Model { public void Edit(string value) { int number = value; } }", "expected int"),
        ("public Class Model { public pure int Read() => 9223372036854775808; }", "outside signed64"),
        ("public Record Value { public int number; } public Class Model { public pure Value Read() => new Value { absent = 1 }; }",
         "unknown or duplicate record"),
        ("public Class Model { public Optional<int> value; }", "unsupported generic"),
        ("public Class Model { public async Task<Result<void,string>> Run() { Result<void,string> result = await Run(); return result; } }",
         "local Task composition is not admitted"),
        ("public Class Model { public pure int Read(bool flag) { if (flag) return 1; } }", "every path"),
    ]
    refusal_cases += [
        ("public Class Model { public pure int Read() => 1 ? 2 : 3; }", "expected bool"),
        ("public Class Model { public pure int Read(bool flag) => flag ? 1 : false; }", "expected int"),
        ("public Record Value { public int number; } public Class Model { public pure Value Read(bool flag) => flag ? Value(1) : 2; }",
         "expected Value"),
        ("public Class Model { public pure Result<int,string> Read(bool flag) => flag ? Result<int,string>.Success(1) : Result<bool,string>.Success(true); }",
         "expected Result<int,string>"),
    ]
    refusal_cases.append(("public Class Model { public void Recurse() { Recurse(); } }", "recursive local call"))
    service_prefix = "public Interface Store { Task<Result<void,string>> Save(CancellationToken token); } "
    refusal_cases += [
        (service_prefix + "public Class Model { private Store service; public Result<Store,string> leak; }",
         "Result payloads must recursively contain owned values"),
        (service_prefix + "public Class Model { private Store service; public pure Result<Store,string> Leak() => Result<Store,string>.Success(service); }",
         "Result payloads must recursively contain owned values"),
        (service_prefix + "public Record Nested { public Result<Result<Store,string>,string> leak; }",
         "Result payloads must recursively contain owned values"),
        (service_prefix + "public Class Model { private Store service; public pure bool Read(CancellationToken token) => service.Save(token).IsSuccess; }",
         "host calls must be directly awaited"),
    ]
    for source, message in refusal_cases:
        refused(args.compiler, source, message)

    # Leaf-first declarations previously hid the actual depth behind a visited
    # set. The oracle deliberately orders helpers in the problematic direction.
    depth_methods = ["public pure int F0() => 1;"]
    depth_methods += [f"public pure int F{index}() => F{index - 1}();" for index in range(1, 34)]
    refused(args.compiler, "public Class Model { " + " ".join(depth_methods) + " }", "call depth limit")
    repeated_methods = ["public pure int F0() => 1;"]
    repeated_methods += [f"public pure int F{index}() => F{index - 1}() + F{index - 1}();"
                         for index in range(1, 18)]
    refused(args.compiler, "public Class Model { " + " ".join(repeated_methods) + " }",
            "evaluation work limit")

    action_methods = ["public int count;", "private void F0() { count = count + 1; }"]
    action_methods += [f"private void F{index}() {{ F{index - 1}(); F{index - 1}(); }}"
                       for index in range(1, 18)]
    refused(args.compiler, "public Class Model { " + " ".join(action_methods) + " }",
            "evaluation work limit")
    layouts = ["public Record R0 { public int value; }"]
    layouts += [f"public Record R{index} {{ public R{index - 1} left; public R{index - 1} right; }}"
                for index in range(1, 10)]
    refused(args.compiler, " ".join(layouts), "expanded value layout limit")

    for raw in [
        '{"protocol":"pipelang.native-model.v1","source":"","source":""}',
        '{"protocol":"wrong","source":""}',
        '{"protocol":"pipelang.native-model.v1","source":"","surprise":true}',
        '{"protocol":"pipelang.native-model.v1","source":42}',
    ]:
        process = invoke(args.compiler, raw=raw)
        assert process.returncode == 2 and process.stdout == "", process
    refused(args.compiler, " " * (64 * 1024 + 1), "source byte limit")
    print(json.dumps({"status": "passed", "semantic_refusals": len(refusal_cases) + 5,
                      "protocol_refusals": 4, "admitted": 2}))


if __name__ == "__main__":
    main()
