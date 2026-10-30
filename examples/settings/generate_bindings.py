#!/usr/bin/env python3
# Legacy scalar/source-v2 compatibility fixture. Current applications use the installed native model SDK.
"""Build the settings fixture from pinned PipeLang tools and admitted metadata."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def closed_object(pairs):
    result = {}
    for name, value in pairs:
        if name in result:
            raise ValueError("Duplicate metadata key: " + name)
        result[name] = value
    return result


def read_metadata(path):
    if path.stat().st_size > 16 << 20:
        raise ValueError("Metadata exceeds fixture capacity")
    return json.loads(path.read_text(), object_pairs_hook=closed_object)


def tool(path, expected):
    if not path.is_absolute() or not path.is_file() or not re.fullmatch(r"[0-9a-f]{64}", expected):
        raise ValueError("Tool requires an absolute path and exact lowercase SHA256")
    if digest(path) != expected:
        raise ValueError("Selected tool checksum mismatch")


def admit_actions(value, source_hash, generated_hash, compiler_hash, emitter_hash, lexer_hash):
    expected = {"contract": "pipelang.native-cpp-export.v1", "profile": "pipelang.cpp.pilot.v1",
                "language_contract": "v0.113.0", "compiler_contract": "pipelang.compiler.v1",
                "source_sha256": source_hash, "generated_sha256": generated_hash,
                "compiler_executable_sha256": compiler_hash}
    for name, expected_value in expected.items():
        if value.get(name) != expected_value:
            raise ValueError("Action metadata mismatch: " + name)
    if value.get("emitter_executable_sha256", "") != emitter_hash or value.get("lexer_executable_sha256", "") != lexer_hash:
        raise ValueError("Selected action compiler components differ")
    functions = value.get("functions")
    if not isinstance(functions, list) or len(functions) != 2:
        raise ValueError("Expected two compiled settings validators")
    boolean = {"kind": "primitive", "primitive": "bool"}
    integer = {"kind": "numeric", "numeric": {"representation": "integer", "bits": 64, "signed": True}}
    symbols = {}
    for function in functions:
        if not isinstance(function, dict):
            raise ValueError("Compiled validator must be an object")
        name = function.get("name")
        if name not in ("IsValidName", "IsValidVolume") or name in symbols:
            raise ValueError("Unexpected or duplicated settings validator")
        primitive = "string" if name == "IsValidName" else "int"
        semantic = {"kind": "primitive", "primitive": primitive}
        parameter = semantic if primitive == "string" else integer
        identity = {"package_id": "pipeview.settings.showcase", "path": "app.root.settings." + name.lower(),
                    "callable": {"parameters": [semantic], "returns": boolean}}
        if (function.get("identity") != identity
                or function.get("parameters") != [{"position": 0, "name": "value", "type": parameter}]
                or function.get("return_type") != boolean
                or not isinstance(function.get("cpp_symbol"), str)
                or not re.fullmatch(r"pipelang::fn_[0-9a-f]{24}", function["cpp_symbol"])):
            raise ValueError("Compiled validator identity or signature differs")
        symbols[name] = function["cpp_symbol"]
    if len(set(symbols.values())) != len(symbols):
        raise ValueError("Compiled validator symbols must be distinct")
    return symbols


def admit_view(value, source_hash, generated_hash, compiler_hash, lexer_hash, syntax="pipelang", view_parser_hash=""):
    expected = {"contract": "pipelang.pipeview.export.v1", "source_contract": "pipelang.pipeview.source.v2",
                "action_contract": "pipelang.pipeview.state.v1", "adapter": "forgepipe.ui.v2",
                "source_sha256": source_hash, "generated_sha256": generated_hash,
                "compiler_executable_sha256": compiler_hash}
    for name, expected_value in expected.items():
        if value.get(name) != expected_value:
            raise ValueError("View metadata mismatch: " + name)
    expected_syntax = "xml" if syntax == "xml" else ""
    expected_format = "header" if syntax == "xml" else ""
    if value.get("output_format", "") != expected_format:
        raise ValueError("Selected view artifact format differs")
    if value.get("source_syntax", "") != expected_syntax:
        raise ValueError("Selected view syntax differs")
    if value.get("lexer_executable_sha256", "") != lexer_hash:
        raise ValueError("Selected view lexer differs")
    if view_parser_hash and syntax != "pipelang":
        raise ValueError("The native view parser supports PipeLang source only")
    if view_parser_hash and not re.fullmatch(r"[0-9a-f]{64}", view_parser_hash):
        raise ValueError("Selected view parser requires an exact lowercase SHA256")
    if value.get("view_parser_executable_sha256", "") != view_parser_hash:
        raise ValueError("Selected view parser differs")
    if not re.fullmatch(r"[0-9a-f]{64}", value.get("state_runtime_sha256", "")):
        raise ValueError("Missing native state runtime identity")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("compiler", "viewc", "source", "view-source", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("compiler-sha256", "viewc-sha256"):
        parser.add_argument("--" + name, required=True)
    for name in ("emitter", "lexer"):
        parser.add_argument("--" + name, type=Path)
        parser.add_argument("--" + name + "-sha256", default="")
    parser.add_argument("--view-parser", type=Path)
    parser.add_argument("--view-parser-sha256", default="")
    parser.add_argument("--view-syntax", choices=("pipelang", "xml"), default="pipelang")
    args = parser.parse_args()
    selected = [(args.compiler, args.compiler_sha256), (args.viewc, args.viewc_sha256)]
    action_components = []
    view_components = []
    for name in ("emitter", "lexer"):
        path, expected = getattr(args, name), getattr(args, name + "_sha256")
        if bool(path) != bool(expected):
            raise ValueError("Component path and checksum must be supplied together")
        if path:
            selected.append((path, expected))
            arguments = ["--cpp-" + name, str(path), "--cpp-" + name + "-sha256", expected]
            action_components += arguments
            if name == "lexer":
                view_components += arguments
    if bool(args.view_parser) != bool(args.view_parser_sha256):
        raise ValueError("View parser path and checksum must be supplied together")
    if args.view_parser:
        if args.view_syntax != "pipelang":
            raise ValueError("The native view parser supports PipeLang source only")
        selected.append((args.view_parser, args.view_parser_sha256))
        view_components += ["--cpp-view-parser", str(args.view_parser),
                            "--cpp-view-parser-sha256", args.view_parser_sha256]
    for path, expected in selected:
        tool(path, expected)
    args.output.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(args.compiler), "--source", str(args.source), "--output-dir", str(args.output),
                    "--package", "pipeview.settings.showcase", "--module", "app.root", *action_components],
                   check=True, timeout=30)
    header = args.output / "PipeViewDocument.hpp"
    subprocess.run([str(args.viewc), "--source", str(args.view_source), "--output", str(header), "--syntax", args.view_syntax, *view_components],
                   check=True, timeout=30)
    symbols = admit_actions(read_metadata(args.output / "bindings.json"), digest(args.source),
                           digest(args.output / "generated.hpp"), args.compiler_sha256,
                           args.emitter_sha256, args.lexer_sha256)
    admit_view(read_metadata(Path(str(header) + ".json")), digest(args.view_source), digest(header),
               args.viewc_sha256, args.lexer_sha256, args.view_syntax, args.view_parser_sha256)
    for path, expected in selected:
        tool(path, expected)
    text = ('#pragma once\n#include "generated.hpp"\n#include "PipeViewDocument.hpp"\n'
            'namespace compiled_ui {\ninline bool IsValidName(const std::string &value)\n{\n'
            '    return ' + symbols['IsValidName'] + '(value);\n}\n'
            'inline bool IsValidVolume(std::int64_t value)\n{\n'
            '    return ' + symbols['IsValidVolume'] + '(value);\n}\n}\n')
    (args.output / "DemoBindings.hpp").write_text(text)


if __name__ == "__main__":
    main()
