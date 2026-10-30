#!/usr/bin/env python3
"""Qualify the existing headless native model subset inside containedexec job.py."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--build-sdk", action="store_true")
    parser.add_argument("--sdk-prefix", type=Path)
    parser.add_argument("--sdk-manifest-sha256")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cxx", type=Path, default=Path("/usr/bin/c++"))
    parser.add_argument("--no-asan", action="store_true")
    parser.add_argument("--estate-limit-mib", type=int, default=512)
    parser.add_argument("--generate", action="store_true", help=argparse.SUPPRESS)
    return parser, parser.parse_args()


def load_module(name, path):
    specification = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


def allocated_bytes(directory):
    allocated = 0
    for path in directory.rglob("*"):
        try:
            if path.is_file():
                allocated += path.stat().st_blocks * 512
        except FileNotFoundError:
            # Temporary compiler files may disappear between enumeration/stat.
            continue
    return allocated


def file_hash(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_identity(root):
    directories = ["sdk/pipeview/model", "src/native/model-compiler",
                   "src/native/model-view-compiler", "src/native/lexer",
                   "src/native/third_party/nlohmann", "tests/native-models",
                   "tests/native-model-views", "tests/containedexec"]
    paths = [path for directory in directories for path in (root / directory).rglob("*")
             if path.is_file() and "__pycache__" not in path.parts]
    for directory in directories:
        if not (root / directory).is_dir():
            raise ValueError("Required qualification source directory missing: " + directory)
    paths.extend(path for path in (root / "LICENSE", root / "NOTICE") if path.is_file())
    return {str(path.relative_to(root)): file_hash(path) for path in sorted(paths)}


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def verify_sdk(args):
    tooling = args.source_root / "sdk/pipeview/model/tooling/sdk_manifest.py"
    manifest = args.sdk_prefix / "share/pipelang-model/sdk-manifest.json"
    verifier = load_module("model_sdk_manifest", tooling)
    identity = verifier.verify(args.sdk_prefix, manifest, args.sdk_manifest_sha256)
    required = {"bin/pipelang-modelc", "bin/pipelang-model-viewc",
                "include/pipelang/model/runtime.hpp", "include/pipelang/model/task.hpp",
                "include/pipelang/model/result.hpp"}
    if not required.issubset({entry["path"] for entry in identity["files"]}):
        raise ValueError("Pinned SDK manifest omits required native tools/runtime")


def generate_semantics(args, harness):
    harness.verify_job()
    group = harness.current_group()
    expected = {"memory.max": 1 << 30, "memory.swap.max": 0, "pids.max": 128}
    if {name: int((group / name).read_text()) for name in expected} != expected:
        raise RuntimeError("Semantic generation requires a containedexec unit")
    verify_sdk(args)
    request = {"protocol": "pipelang.native-model.v1", "namespace": "pipelang_models",
               "source": (args.source_root / "tests/native-models/Models.pipe").read_text()}
    result = subprocess.run([str(args.sdk_prefix / "bin/pipelang-modelc")],
                            input=json.dumps(request), text=True, capture_output=True,
                            timeout=10, check=True)
    if result.stderr or len(result.stdout.encode()) > 4 << 20:
        raise RuntimeError("Unexpected native compiler diagnostics or response size")
    response = json.loads(result.stdout)
    if (set(response) != {"protocol", "ok", "header", "manifest"}
            or response["protocol"] != request["protocol"] or response["ok"] is not True):
        raise RuntimeError("Native semantic fixture generation refused")
    destination = args.output / "semantics"
    destination.mkdir()
    (destination / "Models.hpp").write_text(response["header"])
    write_json(destination / "manifest.json", response["manifest"])


def qualify(args, harness):
    harness.verify_job()
    initial_source = source_identity(args.source_root)
    initial_cxx_hash = file_hash(args.cxx)
    if not args.build_sdk:
        verify_sdk(args)
    args.output.mkdir()
    (args.output / "units").mkdir()
    write_json(args.output / "source-before.json", initial_source)
    limit = args.estate_limit_mib << 20
    receipts = []

    def unit(name, command):
        output = args.output / "units" / name
        invocation = [sys.executable, "-B", str(args.source_root / "tests/containedexec/run.py"),
                      "--output", str(output), "--cache", str(args.output / "cache"),
                      "--timeout", "900", "--memory-high-mib", "700", "--",
                      *map(str, command)]
        if allocated_bytes(args.output) >= limit:
            raise RuntimeError("Qualification estate budget exhausted before " + name)
        process = subprocess.Popen(invocation, cwd=args.source_root)
        try:
            while process.poll() is None:
                if allocated_bytes(args.output) >= limit:
                    raise RuntimeError("Qualification estate budget exhausted during " + name)
                time.sleep(0.2)
        finally:
            if process.poll() is None:
                process.send_signal(signal.SIGINT)
                process.wait(timeout=30)
        receipt_path = Path(str(output) + ".json")
        receipt = json.loads(receipt_path.read_text())
        if (process.returncode != 0 or receipt.get("exit") != 0
                or receipt.get("tree_removed") is not True
                or receipt.get("outcome") != "completed"):
            raise RuntimeError("Contained qualification unit failed: " + name)
        allocated = allocated_bytes(args.output)
        if allocated >= limit:
            raise RuntimeError("Qualification estate budget exhausted after " + name)
        receipts.append({"name": name, "receipt": str(receipt_path),
                         "sha256": file_hash(receipt_path)})
        write_json(args.output / "progress.json", {"completed": receipts, "allocated_bytes": allocated})

    python = [sys.executable, "-B"]
    tests = args.source_root / "tests"
    if args.build_sdk:
        args.sdk_prefix = args.output / "sdk"
        build = args.output / "sdk-build"
        unit("sdk-configure", ["cmake", "-S", args.source_root / "sdk/pipeview/model",
                               "-B", build, "-DCMAKE_BUILD_TYPE=Release",
                               "-DCMAKE_CXX_COMPILER=" + str(args.cxx)])
        unit("sdk-build", ["cmake", "--build", build, "--parallel", "1"])
        unit("sdk-install", ["cmake", "--install", build, "--prefix", args.sdk_prefix])
        args.sdk_manifest_sha256 = file_hash(args.sdk_prefix / "share/pipelang-model/sdk-manifest.json")
        verify_sdk(args)
    compiler = args.sdk_prefix / "bin/pipelang-modelc"
    view_compiler = args.sdk_prefix / "bin/pipelang-model-viewc"
    unit("model-protocol", [*python, tests / "native-models/protocol.py", "--compiler", compiler])
    self_arguments = ["--source-root", args.source_root, "--sdk-prefix", args.sdk_prefix,
                      "--sdk-manifest-sha256", args.sdk_manifest_sha256, "--output", args.output]
    unit("semantics-generate", [*python, Path(__file__).resolve(), *self_arguments, "--generate"])
    semantics = args.output / "semantics"
    compile_arguments = [args.cxx, "-std=c++20", "-pthread", "-Wall", "-Wextra", "-Werror",
                         "-I" + str(semantics), "-I" + str(args.sdk_prefix / "include"),
                         tests / "native-models/semantics.cpp"]
    unit("semantics-build", [*compile_arguments, "-O2", "-o", semantics / "test"])
    unit("semantics-run", [semantics / "test"])
    fundamentals = args.output / "fundamentals"
    unit("fundamentals-generate", [*python, tests / "native-models/fundamentals.py",
                                   "--compiler", compiler, "--output", fundamentals])
    fundamentals_compile = [args.cxx, "-std=c++20", "-pthread", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(fundamentals), "-I" + str(args.sdk_prefix / "include"),
                            tests / "native-models/fundamentals.cpp"]
    unit("fundamentals-build", [*fundamentals_compile, "-O2", "-o", fundamentals / "test"])
    unit("fundamentals-run", [fundamentals / "test"])
    unit("numeric-guard", [*python, tests / "native-models/numeric-guard.py", "--cxx", args.cxx,
                           "--include", args.sdk_prefix / "include",
                           "--fixture", args.output / "NumericDefaults.hpp"])
    iteration = args.output / "iteration"
    unit("iteration-generate", [*python, tests / "native-models/iteration-tests.py",
                                "--compiler", compiler, "--output", iteration])
    iteration_compile = [args.cxx, "-std=c++20", "-pthread", "-Wall", "-Wextra", "-Werror",
                         "-I" + str(iteration), "-I" + str(args.sdk_prefix / "include"),
                         tests / "native-models/iteration-tests.cpp"]
    unit("iteration-build", [*iteration_compile, "-O2", "-o", iteration / "test"])
    unit("iteration-run", [iteration / "test"])
    composition = args.output / "async-composition"
    unit("composition-generate", [*python, tests / "native-models/async-composition.py",
                                  "--compiler", compiler, "--output", composition])
    composition_compile = [args.cxx, "-std=c++20", "-pthread", "-Wall", "-Wextra", "-Werror",
                           "-I" + str(composition), "-I" + str(args.sdk_prefix / "include"),
                           tests / "native-models/async-composition.cpp"]
    unit("composition-build", [*composition_compile, "-O2", "-o", composition / "test"])
    unit("composition-run", [composition / "test"])
    cleanup = args.output / "failure-cleanup"
    unit("cleanup-generate", [*python, tests / "native-models/failure-cleanup.py",
                              "--compiler", compiler, "--output", cleanup])
    cleanup_compile = [args.cxx, "-std=c++20", "-pthread", "-Wall", "-Wextra", "-Werror",
                       "-I" + str(cleanup), "-I" + str(args.sdk_prefix / "include"),
                       tests / "native-models/failure-cleanup.cpp"]
    unit("cleanup-build", [*cleanup_compile, "-O2", "-o", cleanup / "test"])
    unit("cleanup-run", [cleanup / "test"])
    unit("view-protocol", [*python, tests / "native-model-views/test_protocol.py",
                           "--compiler", view_compiler, "--output", args.output / "view"])
    unit("cross-tool", [*python, tests / "native-model-views/test_cross_tool.py",
                        "--model-compiler", compiler, "--view-compiler", view_compiler])
    unit("styles", [*python, tests / "native-model-views/test_styles.py",
                    "--compiler", view_compiler, "--output", args.output / "styles"])
    unit("style-inspection", [*python, tests / "native-model-views/test_style_inspection.py",
                             "--compiler", view_compiler])
    unit("style-generation", [*python, tests / "native-model-views/test_style_generation.py",
                             "--sdk-prefix", args.sdk_prefix, "--output", args.output / "style-generation"])
    unit("sdk-admission", [*python, args.source_root / "sdk/pipeview/model/tooling/test_generate_bindings.py"])
    if not args.no_asan:
        unit("asan-build", [*compile_arguments, "-O1", "-g", "-fsanitize=address",
                            "-fno-omit-frame-pointer", "-o", semantics / "test-asan"])
        unit("asan-run", ["/usr/bin/env", "ASAN_OPTIONS=detect_leaks=1:halt_on_error=1",
                          semantics / "test-asan"])
        unit("fundamentals-asan-build", [*fundamentals_compile, "-O1", "-g", "-fsanitize=address",
                                         "-fno-omit-frame-pointer", "-o", fundamentals / "test-asan"])
        unit("fundamentals-asan-run", ["/usr/bin/env", "ASAN_OPTIONS=detect_leaks=1:halt_on_error=1",
                                       fundamentals / "test-asan"])
        unit("iteration-asan-build", [*iteration_compile, "-O1", "-g", "-fsanitize=address",
                                      "-fno-omit-frame-pointer", "-o", iteration / "test-asan"])
        unit("iteration-asan-run", ["/usr/bin/env", "ASAN_OPTIONS=detect_leaks=1:halt_on_error=1",
                                    iteration / "test-asan"])
        unit("composition-asan-build", [*composition_compile, "-O1", "-g", "-fsanitize=address",
                                        "-fno-omit-frame-pointer", "-o", composition / "test-asan"])
        unit("composition-asan-run", ["/usr/bin/env", "ASAN_OPTIONS=detect_leaks=1:halt_on_error=1",
                                      composition / "test-asan"])
        unit("cleanup-asan-build", [*cleanup_compile, "-O1", "-g", "-fsanitize=address",
                                    "-fno-omit-frame-pointer", "-o", cleanup / "test-asan"])
        unit("cleanup-asan-run", ["/usr/bin/env", "ASAN_OPTIONS=detect_leaks=1:halt_on_error=1",
                                  cleanup / "test-asan"])
    verify_sdk(args)
    final_source = source_identity(args.source_root)
    write_json(args.output / "source-after.json", final_source)
    if final_source != initial_source:
        raise RuntimeError("Qualification source changed during execution")
    if file_hash(args.cxx) != initial_cxx_hash:
        raise RuntimeError("Selected C++ compiler changed during execution")
    fixture_paths = [tests / "native-models/Models.pipe", tests / "native-models/semantics.cpp",
                     tests / "native-models/protocol.py", tests / "native-model-views/Fixture.pipe",
                     tests / "native-model-views/FixtureModel.pipe",
                     tests / "native-model-views/test_protocol.py",
                     tests / "native-model-views/test_cross_tool.py",
                     tests / "native-model-views/test_styles.py",
                     tests / "native-model-views/test_style_inspection.py",
                     tests / "native-model-views/test_style_generation.py",
                     tests / "native-model-views/StyleTest.cpp",
                     args.source_root / "sdk/pipeview/model/tooling/test_generate_bindings.py"]
    fixture_paths.extend(tests / "native-models" / name for name in
                         ("Fundamentals.pipe", "fundamentals.cpp", "fundamentals.py", "numeric-guard.py",
                          "iteration-tests.pipe", "iteration-tests.cpp", "iteration-tests.py",
                          "async-composition.pipe", "async-composition.cpp", "async-composition.py",
                          "FailureCleanup.pipe", "failure-cleanup.cpp", "failure-cleanup.py"))
    fundamentals_output = (args.output / "units/fundamentals-generate.output").read_text()
    fundamentals_cases = json.loads(fundamentals_output.strip().splitlines()[-1])
    if fundamentals_cases.get("status") != "passed" or fundamentals_cases.get("semantic_refusals") != 28:
        raise RuntimeError("Fundamentals oracle case inventory changed")
    iteration_output = (args.output / "units/iteration-generate.output").read_text()
    iteration_cases = json.loads(iteration_output.strip().splitlines()[-1])
    if iteration_cases != {"status": "passed", "refusals": 12}:
        raise RuntimeError("Iteration oracle case inventory changed")
    composition_output = (args.output / "units/composition-generate.output").read_text()
    composition_cases = json.loads(composition_output.strip().splitlines()[-1])
    if composition_cases != {"status": "passed", "refusals": 10}:
        raise RuntimeError("Async composition oracle case inventory changed")
    cleanup_output = (args.output / "units/cleanup-generate.output").read_text()
    cleanup_cases = json.loads(cleanup_output.strip().splitlines()[-1])
    if cleanup_cases != {"status": "passed", "refusals": 21}:
        raise RuntimeError("Failure/cleanup oracle case inventory changed")
    protocol_output = (args.output / "units/model-protocol.output").read_text()
    protocol_cases = json.loads(protocol_output.strip().splitlines()[-1])
    if protocol_cases != {"status": "passed", "semantic_refusals": 25,
                          "protocol_refusals": 4, "admitted": 2}:
        raise RuntimeError("Model protocol oracle case inventory changed")
    sdk_test_output = (args.output / "units/sdk-admission.output").read_text()
    if not re.search(r"\bRan 15 tests? in ", sdk_test_output) or "\nOK\n" not in sdk_test_output:
        raise RuntimeError("SDK tooling oracle case inventory changed")
    style_output = (args.output / "units/styles.output").read_text()
    style_cases = json.loads(style_output.strip().splitlines()[-1])
    if style_cases != {"status": "passed", "style_refusals": 43, "unstyled_artifact_parity": False}:
        raise RuntimeError("Style oracle case inventory changed")
    inspection_output = (args.output / "units/style-inspection.output").read_text()
    inspection_cases = json.loads(inspection_output.strip().splitlines()[-1])
    if inspection_cases != {"status": "passed", "inspection_refusals": 9, "default_wire_parity": False}:
        raise RuntimeError("Style inspection oracle case inventory changed")
    generation_output = (args.output / "units/style-generation.output").read_text()
    generation_cases = json.loads(generation_output.strip().splitlines()[-1])
    if generation_cases != {"status": "passed", "mode_sequences": ["default", "enabled", "disabled", "enabled"], "cmake_modes": ["disabled", "enabled", "disabled"], "refusal_count": 2}:
        raise RuntimeError("Installed style generation oracle case inventory changed")
    write_json(args.output / "acceptance.json", {
        "status": "passed", "source_root": str(args.source_root),
        "sdk_prefix": str(args.sdk_prefix), "sdk_manifest_sha256": args.sdk_manifest_sha256,
        "cxx": str(args.cxx), "cxx_sha256": initial_cxx_hash,
        "mode": "source-build" if args.build_sdk else "installed-artifact",
        "source_sha256": initial_source,
        "asan": not args.no_asan, "units": receipts,
        "cases": {"failure_cleanup": cleanup_cases, "composition": composition_cases, "iteration": iteration_cases, "fundamentals": fundamentals_cases, "model_protocol": protocol_cases, "sdk_tooling": 15, "styles": style_cases, "style_inspection": inspection_cases, "style_generation": generation_cases},
        "fixture_sha256": {str(path.relative_to(args.source_root)): file_hash(path)
                           for path in fixture_paths},
        "allocated_bytes": allocated_bytes(args.output), "estate_limit_bytes": limit,
        "limits": "1 GiB/unit, 800 MiB proactive, zero swap, 2 GiB aggregate",
        "scope": ("Source-built SDK and headless native acceptance" if args.build_sdk
                  else "Selected installed SDK artifact against current source fixtures; local compiler/runtime implementation is not qualified"),
        "excluded": "ForgePipe adapter execution, UI, Go, full-suite, performance, platform and release qualification",
    })


def main():
    parser, args = arguments()
    if args.build_sdk:
        if args.sdk_prefix is not None or args.sdk_manifest_sha256 is not None:
            parser.error("build-sdk excludes installed SDK selections")
    elif args.sdk_prefix is None or args.sdk_manifest_sha256 is None:
        parser.error("select build-sdk or both sdk-prefix and sdk-manifest-sha256")
    path_names = ["source_root", "output", "cxx"]
    if args.sdk_prefix is not None:
        path_names.append("sdk_prefix")
    for name in path_names:
        path = getattr(args, name)
        if not path.is_absolute():
            parser.error(name.replace("_", "-") + " must be absolute")
        setattr(args, name, path.resolve())
    if not 1 <= args.estate_limit_mib <= 8192:
        parser.error("estate-limit-mib must be between 1 and 8192")
    if args.output == args.source_root or args.source_root in args.output.parents:
        parser.error("output and cache must be outside the source tree")
    if args.sdk_prefix is not None and (args.output == args.sdk_prefix or args.sdk_prefix in args.output.parents):
        parser.error("output must be outside the installed SDK")
    if not args.generate and args.output.exists():
        parser.error("output must be a fresh, nonexistent directory")
    if not args.cxx.is_file() or not os.access(args.cxx, os.X_OK):
        parser.error("cxx must select an executable compiler")
    sys.path.insert(0, str(args.source_root / "tests/containedexec"))
    import job
    if "CONTAINED_JOB_GROUP" not in os.environ:
        if args.generate:
            raise RuntimeError("Semantic generation requires existing containment")
        invocation = [sys.executable, "-B", str(args.source_root / "tests/containedexec/job.py"),
                      "--output", str(args.output) + "-job", "--timeout", "10800", "--",
                      sys.executable, "-B", str(Path(__file__).resolve()), *sys.argv[1:]]
        return subprocess.call(invocation, cwd=args.source_root)
    job.verify_job()
    if args.generate:
        generate_semantics(args, job)
    else:
        qualify(args, job)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print("native-model qualification failed: " + str(error), file=sys.stderr)
        sys.exit(1)
