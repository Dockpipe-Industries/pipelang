#!/usr/bin/env python3
"""Generate fundamentals fixtures and check refusals inside job + unit containment.

This driver invokes only the explicitly selected compiler. The parent separately
compiles fundamentals.cpp with the emitted Fundamentals.hpp inside containment.
"""
import argparse
import json
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
        raise RuntimeError("fundamentals tests require a containedexec unit")


def invoke(compiler, source):
    request = {"protocol": "pipelang.native-model.v1", "source": source,
               "namespace": "pipelang_models"}
    process = subprocess.run([str(compiler)], input=json.dumps(request), text=True,
                             capture_output=True, timeout=10, check=False)
    assert process.returncode == 0, process.stderr
    assert len(process.stdout) <= 4 << 20
    assert len(process.stderr) <= 65536
    result = json.loads(process.stdout)
    assert result["protocol"] == "pipelang.native-model.v1", result
    return result


def refused(compiler, source):
    result = invoke(compiler, source)
    assert result["ok"] is False, result
    assert "header" not in result and "manifest" not in result
    assert result["diagnostics"], result
    for diagnostic in result["diagnostics"]:
        assert isinstance(diagnostic["offset"], int), diagnostic
        assert diagnostic["offset"] >= 0, diagnostic
        assert diagnostic["message"], diagnostic


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    verify_containment()
    if not args.compiler.is_absolute() or not args.output.is_absolute():
        parser.error("absolute compiler and output paths required")
    source = Path(__file__).with_name("Fundamentals.pipe").read_text()
    result = invoke(args.compiler, source)
    assert result["ok"] is True, result
    assert set(result) == {"protocol", "ok", "header", "manifest"}, result
    manifest = result["manifest"]
    assert manifest["protocol"] == "pipelang.native-model-manifest.v1"
    model = next(item for item in manifest["classes"] if item["name"] == "Fundamentals")
    fields = {item["name"]: item["type"] for item in model["fields"]}
    assert fields["stored"] == "float", fields
    assert fields["label"] == "Optional<Label>", fields

    cases = [
        "public Class M { public float value = 1; }",
        "public Class M { public int value = 1.0; }",
        "public Class M { public pure float Read(float value) => value + 1; }",
        "public Class M { public pure bool Read(float value) => value < 1; }",
        "public Class M { public pure float Read(bool flag) => flag ? 1.0 : 1; }",
        "public Class M { public pure float Read() => 1e+; }",
        "public Class M { public pure bool Read() => float.IsFinite(1); }",
        "public Class M { public pure bool Read() => float.IsFinite(true); }",
        "public Class M { public pure bool Read() => float.IsFinite(); }",
        "public Class M { public pure float Read() => float.Unknown(1.0); }",
        "public Class M { public Optional<float> value = Optional<float>.Some(1); }",
        "public Class M { public Optional<float> value = Optional<int>.None(); }",
        "public Class M { public Optional<void> value; }",
        "public Class M { public Optional<CancellationToken> value; }",
        "public Class M { public Optional<int,string> value; }",
        "public Class M { public pure int Read() => Optional<int>.Some().Value; }",
        "public Class M { public pure int Read() => Optional<int>.None(1).Value; }",
        "public Class M { public pure int Read() { { int local = 1; } return local; } }",
        "public Class M { public pure int Read() { int local = 1; { int local = 2; } return local; } }",
        "public Class M { public pure int Read(int local) { { int local = 2; } return local; } }",
        "public Class M { public pure int Read(bool flag) { { if (flag) return 1; } } }",
        "public Record Recursive { public Optional<Recursive> value; }",
        "public Record Recursive { public Result<Optional<Recursive>,string> value; }",
    ]
    service = "public Interface Store { Task<Result<void,string>> Save(CancellationToken token); } "
    cases += [
        service + "public Record R { public Optional<Store> value; }",
        service + "public Record R { public Optional<Result<Store,string>> value; }",
        service + "public Record R { public Result<Optional<Store>,string> value; }",
        "public Record R { public Optional<Result<Optional<CancellationToken>,string>> value; }",
    ]
    # Decimal syntax is supported, but finite literal admission must reject an
    # overflow-sized decimal without relying on exponent lexical extensions.
    cases.append("public Class M { public float value = " + "9" * 400 + ".0; }")
    for refusal in cases:
        refused(args.compiler, refusal)

    defaults = invoke(args.compiler,
        "public Record NumericDefaults { public bool finite = float.IsFinite(0.0); } "
        "public Class DefaultOwner { public NumericDefaults value; }")
    assert defaults["ok"] is True, defaults
    assert "#include <pipelang/model/numeric.hpp>" in defaults["header"]
    (args.output.parent / "NumericDefaults.hpp").write_text(defaults["header"])
    for literal in ("1e309", "1e-999"):
        refused(args.compiler, "public Class M { public float value = " + literal + "; }")
    deep = "public Class M { public void Go() { " + "if (true) " * 5000 + "return; } }"
    refused(args.compiler, deep)
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "Fundamentals.hpp").write_text(result["header"])
    (args.output / "Fundamentals.manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps({"status": "passed", "semantic_refusals": len(cases),
                      "header": str(args.output / "Fundamentals.hpp")}))


if __name__ == "__main__":
    main()
