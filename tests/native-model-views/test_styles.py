#!/usr/bin/env python3
"""Independent typed-style admission and generated artifact oracle."""
import argparse
import json
from pathlib import Path
import shutil

from test_protocol import manifest, run


BASE = '''style Theme {
    token inset: length = 16px;
    token ink: color = "#DDEEFFff";
    padding: inset;
    gap: 8px;
    controlHeight: 42px;
    text: ink;
    focus: "#AABBCCff";
    disabledText: "#667788ff";
}
style Compact extends Theme {
    override token inset: length = 10.5px;
    override gap: 4px;
    background: "#102030ff";
}
'''


def request(source):
    return {"protocol": "pipelang.native-model-view.v1", "source": source,
            "model_manifest": manifest(), "namespace": "pipelang_models", "model_header": "Model.hpp"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--baseline-compiler", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    fixture = Path(__file__).resolve().parent
    source = (fixture / "Fixture.pipe").read_text()
    plain = run(args.compiler, request(source))
    assert plain["ok"], plain
    if args.baseline_compiler:
        assert plain == run(args.baseline_compiler, request(source)), "unstyled artifact changed"
    selected = source.replace("FixtureModel) {", "FixtureModel) style Compact {")
    response = run(args.compiler, request(BASE + selected))
    assert response["ok"], response
    expected = {"padding": "10.5", "gap": "4", "controlHeight": "42",
                "text": "0xDDEEFFffU", "focus": "0xAABBCCffU",
                "disabledText": "0x667788ffU", "background": "0x102030ffU"}
    for name, value in expected.items():
        assert f"document.style.{name} = {value};" in response["header"], (name, response)
    style_lines = [line for line in response["header"].splitlines(keepends=True) if "document.style." not in line]
    assert "".join(style_lines) == plain["header"], "style changed model, actions or projection generation"
    assert response["manifest"] == plain["manifest"], "style altered typed model/view identity"
    forward = '''style Child extends Parent { override token accent: color = "#11223344"; }
style Parent { token accent: color = later; token later: color = "#AABBCCDD"; text: accent; }
'''
    accepted = run(args.compiler, request(forward + source.replace("FixtureModel) {", "FixtureModel) style Child {")))
    assert accepted["ok"] and "document.style.text = 0x11223344U;" in accepted["header"], accepted
    unselected = run(args.compiler, request(BASE + source))
    assert unselected == plain, "unselected styles must not act as a global cascade"
    ranges = {"padding": (0, 64), "gap": (0, 64), "labelHeight": (1, 128),
              "controlHeight": (1, 256), "fontPixelHeight": (7, 56),
              "textInset": (0, 64), "borderWidth": (0, 8)}
    for name, (minimum, maximum) in ranges.items():
        for value in (minimum, maximum):
            boundary = f"style Boundary {{ {name}: {value}px; }}\n"
            admitted = run(args.compiler, request(boundary + source.replace("FixtureModel) {", "FixtureModel) style Boundary {")))
            assert admitted["ok"], (name, value, admitted)
    refusals = [
        ('style Bad { paddding: 1px; }', "unknown"),
        ('style Bad { scale: 2px; }', "unsupported"),
        ('style Bad { padding: 1; }', "identifier"),
        ('style Bad { gap: 1em; }', "px"),
        ('style Bad { gap: -1px; }', "literal"),
        ('style Bad { gap: 65px; }', "range"),
        ('style Bad { fontPixelHeight: 6px; }', "range"),
        ('style Bad { text: "#ffffff"; }', "#RRGGBBAA"),
        ('style Bad { text: "#GGFFFFFF"; }', "#RRGGBBAA"),
        ('style Bad { text: 2px; }', "type"),
        ('style Bad { padding: "#000000ff"; }', "type"),
        ('style Bad { token value: bool = 1px; }', "type"),
        ('style Bad { token a: length = missing; }', "unknown"),
        ('style Bad { token a: length = b; token b: length = a; }', "cycle"),
        ('style Bad extends Missing {}', "unknown"),
        ('style A extends B {} style B extends A {}', "cycle"),
        ('style Bad {} style Bad {}', "duplicate"),
        ('style Bad { gap: 1px; gap: 2px; }', "duplicate"),
        ('style Bad { token a: length = 1px; token a: length = 2px; }', "duplicate"),
        ('style A { gap: 1px; } style B extends A { gap: 2px; }', "override"),
        ('style A { token a: length = 1px; } style B extends A { token a: length = 2px; }', "override"),
        ('style Bad { override gap: 1px; }', "inherited"),
        ('style A { token a: length = 1px; } style B extends A { override token a: color = "#000000ff"; }', "type"),
        ('style Bad { Button { text: "#000000ff"; } }', "unsupported"),
        ('style Bad { hover: "#000000ff"; }', "unsupported"),
        ('style Bad { token unused: color = 1px; }', "type"),
    ]
    refusals.append((" ".join(f"style Style{i} {{}}" for i in range(33)), "limit"))
    refusals.append(('style Bad { ' + ' '.join(f'token t{i}: length = 1px;' for i in range(33)) + ' }', "limit"))
    refusals.append((" ".join(f"style Level{i}" + (f" extends Level{i + 1}" if i < 16 else "") + " {}" for i in range(17)), "depth"))
    refusals.append(('style Bad { ' + ' '.join(f'token t{i}: length = ' + (f't{i + 1}' if i < 16 else '1px') + ';' for i in range(17)) + ' }', "depth"))
    for name, (minimum, maximum) in ranges.items():
        refusals.append((f"style Bad {{ {name}: {maximum + 1}px; }}", "range"))
        if minimum > 0:
            refusals.append((f"style Bad {{ {name}: {minimum - 1}px; }}", "range"))
    inherited_tokens = " ".join(f"token t{i}: length = 1px;" for i in range(32))
    refusals.append(("style A { " + inherited_tokens + " } style B extends A { token extra: length = 1px; }", "effective"))
    token_chain = " ".join(f"token t{i}: length = " + (f"t{i + 1}" if i < 15 else "1px") + ";" for i in range(16))
    work_graph = "style Base { " + token_chain + " } " + " ".join(f"style Child{i} extends Base {{}}" for i in range(31))
    refusals.append((work_graph, "work"))
    for declaration, message in refusals:
        rejected = run(args.compiler, request(declaration + "\n" + source))
        assert rejected["ok"] is False, declaration
        diagnostic = rejected["diagnostics"][0]
        assert diagnostic["code"] == "PMV2001" and message in diagnostic["message"], (declaration, rejected)
        assert 0 <= diagnostic["start"] < diagnostic["end"] <= len(declaration.encode()), rejected
    missing = run(args.compiler, request(source.replace("FixtureModel) {", "FixtureModel) style Missing {")))
    assert not missing["ok"] and "unknown" in missing["diagnostics"][0]["message"], missing
    args.output.mkdir(parents=True, exist_ok=False)
    for name, view_source in [("PlainView", source), ("StyledView", BASE + selected)]:
        compiled = run(args.compiler, request(view_source.replace("FixtureView", name)))
        assert compiled["ok"], compiled
        (args.output / (name + ".hpp")).write_text(compiled["header"])
    shutil.copy2(fixture / "Model.hpp", args.output / "Model.hpp")
    shutil.copy2(fixture / "StyleTest.cpp", args.output / "StyleTest.cpp")
    (args.output / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.21.1)
project(PipeViewStyleOracle LANGUAGES CXX)
find_package(ForgePipe 0.9.0 EXACT CONFIG REQUIRED COMPONENTS UI)
find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED)
add_executable(style-oracle StyleTest.cpp)
target_link_libraries(style-oracle PRIVATE ForgePipe::UI PipeLang::ModelRuntime)
enable_testing()
add_test(NAME TypedStyleOracle COMMAND style-oracle)
''')
    print(json.dumps({"status": "passed", "style_refusals": len(refusals) + 1,
                      "unstyled_artifact_parity": bool(args.baseline_compiler)}))


if __name__ == "__main__":
    main()
