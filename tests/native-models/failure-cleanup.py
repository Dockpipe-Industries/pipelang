#!/usr/bin/env python3
"""Independent contained failure/cleanup oracle; generate, never compile."""
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
    response = invoke(args.compiler, Path(__file__).with_name("FailureCleanup.pipe").read_text())
    assert response["ok"] is True, response
    assert set(response) == {"protocol", "ok", "header", "manifest"}
    assert response["manifest"]["protocol"] == "pipelang.native-model-manifest.v1"

    cases = [
        "public Class M { public void Run() { throw; } }",
        "public Class M { public void Run() { throw 1; } }",
        "public Class M { public void Run() { throw \"text\"; } }",
        "public Class M { public void Run() { try {} } }",
        "public Class M { public void Run() { try {} catch (string error) {} } }",
        "public Class M { public void Run() { try {} catch (Error) {} } }",
        "public Class M { public void Run() { try {} catch (Error error) {} string text = error.Message; } }",
        "public Class M { public void Run() { int error = 0; try {} catch (Error error) {} } }",
        "public Class M { public void Run() { Error error = Error(1); } }",
        "public Class M { public void Run() { Error error = Error(\"owned\"); error.Message = \"mutated\"; } }",
        "public Class M { public void Run() { try {} finally { return; } } }",
        "public Class M { public void Run() { while (true) { try {} finally { break; } } } }",
        "public Class M { public void Run() { while (true) { try {} finally { continue; } } } }",
        "public Class M { public void Run() { try { int scoped = 1; } finally { int value = scoped; } } }",
        "public Class M { public void Run() { try {} catch (Error error) {} finally { string text = error.Message; } } }",
        "public Class M { public void Run() { try {} finally { throw; } } }",
        "public Class M { public void Run() { try { throw Error(\"x\"); } catch (Error error) {} throw; } }",
        "public Class M { private void Cycle() { try {} finally { Cycle(); } } public void Run() { Cycle(); } }",
    ]
    child = "private async Task<Result<int,string>> Child() { return Result<int,string>.Success(1); }"
    cases += [
        "public Class M { " + child + " public async Task<Result<int,string>> Run() { try {} finally { Result<int,string> result = await Child(); } return Result<int,string>.Success(1); } }",
        "public Class M { " + child + " public async Task<Result<int,string>> Run() { try {} finally { if (true) { Result<int,string> result = await Child(); } } return Result<int,string>.Success(1); } }",
        "public Class M { " + child + " public int Run() { try { throw Error(\"x\"); } catch (Error error) { Result<int,string> result = await Child(); return result.Value; } } }",
    ]
    for source in cases:
        refusal = invoke(args.compiler, source)
        assert refusal["ok"] is False and refusal["diagnostics"], refusal
        assert "header" not in refusal and "manifest" not in refusal
        for diagnostic in refusal["diagnostics"]:
            assert isinstance(diagnostic["offset"], int) and diagnostic["offset"] >= 0
            assert diagnostic["message"]
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "FailureCleanup.hpp").write_text(response["header"])
    (args.output / "FailureCleanup.manifest.json").write_text(json.dumps(response["manifest"], indent=2) + "\n")
    print(json.dumps({"status": "passed", "refusals": len(cases)}))


if __name__ == "__main__":
    main()
