#!/usr/bin/env python3
"""Bounded protocol refusals and generation; execution belongs in containedexec."""
import argparse
import copy
import json
from pathlib import Path
import subprocess


def method(name, return_type, parameters=(), pure=False, asynchronous=False):
    return {
        "name": name,
        "returnType": return_type,
        "parameters": [{"name": name, "type": kind} for name, kind in parameters],
        "pure": pure,
        "async": asynchronous,
    }


def manifest():
    return {
        "protocol": "pipelang.native-model-manifest.v1",
        "namespace": "pipelang_models",
        "records": [{"name": "Values", "fields": [
            {"name": "Name", "type": "string"},
            {"name": "Enabled", "type": "bool"},
            {"name": "Volume", "type": "int"},
        ]}],
        "enums": [],
        "interfaces": [],
        "classes": [{"name": "FixtureModel", "constructor": {"parameters": [{"name": "initial", "type": "Values"}]}, "fields": [
            {"name": "Draft", "type": "Values"},
            {"name": "Saving", "type": "bool"},
            {"name": "Error", "type": "string"},
        ], "methods": [
            method("CanSave", "bool", pure=True),
            method("SetName", "void", [("value", "string")]),
            method("SetEnabled", "void", [("value", "bool")]),
            method("SetVolume", "void", [("value", "int")]),
            method("Save", "Task<Result<void,string>>", [("cancellation", "CancellationToken")], asynchronous=True),
            method("Fail", "Result<void,string>"),
            method("Cancel", "void"),
        ]}],
    }


def run(compiler, request):
    encoded = json.dumps(request).encode() if isinstance(request, dict) else request
    result = subprocess.run([str(compiler)], input=encoded, capture_output=True, timeout=5, check=True)
    response = json.loads(result.stdout)
    assert response["protocol"] == "pipelang.native-model-view.v1"
    return response


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    fixtures = Path(__file__).resolve().parent
    source = (fixtures / "Fixture.pipe").read_text()
    request = {
        "protocol": "pipelang.native-model-view.v1",
        "source": source,
        "model_manifest": manifest(),
        "namespace": "pipelang_models",
        "model_header": "Model.hpp",
    }
    compiled = run(args.compiler, request)
    assert compiled["ok"], compiled
    assert compiled["manifest"]["adapter"] == "FixtureViewAdapter"
    assert "admitActionEvent" in compiled["header"] and "applyProjection" in compiled["header"]
    assert "std::nullopt" in compiled["header"] and "inspection()" in compiled["header"]
    assert '.binding = "model.Draft.Name";' in compiled["header"]
    reordered = copy.deepcopy(request)
    reordered["source"] = source.replace('Column(id: "root") {', 'Column(id: "root") { Label(id: "unrelated");')
    stable = run(args.compiler, reordered)
    assert stable["ok"] and '.binding = "model.Draft.Name";' in stable["header"]
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "View.hpp").write_text(compiled["header"])
    for name in ("Model.hpp", "GeneratedTest.cpp", "CMakeLists.txt"):
        (args.output / name).write_text((fixtures / name).read_text())

    def refuses(candidate):
        response = run(args.compiler, candidate)
        assert response["ok"] is False, candidate
        assert "header" not in response and response["diagnostics"]
        for diagnostic in response["diagnostics"]:
            assert diagnostic["message"] and 0 <= diagnostic["start"] <= diagnostic["end"] <= 65536

    mutations = [
        ("model.Draft.Name", "model.Draft.Missing"),
        ("model.Draft.Name", "model.Saving"),
        ("model.CanSave()", "model.Fail()"),
        ("model.CanSave()", "model.Draft.Name"),
        ("changed: model.SetName", "clicked: model.SetName"),
        ("clicked: model.Save", "changed: model.Save"),
        ("changed: model.SetName", "changed: model.SetVolume"),
        ("clicked: model.Save", "clicked: model.SetName"),
        ("model: FixtureModel", "alias: FixtureModel"),
        ("model.Draft.Name", "alias.Draft.Name"),
        ("sensitive: true", "sensitive: bind model.Saving"),
        ("id: \"save\"", "id: \"name\""),
        ("Column(id: \"root\")", "Column(id: \"root\", enabled: false)"),
        ("Label(id: \"error\", value: bind model.Error)", "Label(id: \"error\", clicked: model.Fail)"),
        ("TextField(id: \"name\"", "Slider(id: \"name\""),
        ("view FixtureView", "view class"),
        ("changed: model.SetName", "changed: model.SetName, changed: model.SetName"),
    ]
    for old, new in mutations:
        candidate = copy.deepcopy(request)
        candidate["source"] = source.replace(old, new)
        assert candidate["source"] != source
        refuses(candidate)
    for key, value in [("namespace", "unsafe::class"), ("namespace", "other"),
                       ("model_header", "../Model.hpp"), ("model_header", 'Model.hpp\"\n#error leak'),
                       ("source", " " * 65537)]:
        candidate = copy.deepcopy(request)
        candidate[key] = value
        refuses(candidate)
    candidate = copy.deepcopy(request)
    candidate["model_manifest"]["classes"][0]["methods"][0]["pure"] = False
    refuses(candidate)
    candidate = copy.deepcopy(request)
    candidate["model_manifest"]["classes"][0]["methods"][4]["async"] = False
    refuses(candidate)
    candidate = copy.deepcopy(request)
    candidate["model_manifest"]["classes"][0]["fields"][0]["name"] = "Draft;attack"
    refuses(candidate)
    candidate = copy.deepcopy(request)
    candidate["model_manifest"]["classes"][0]["fields"].append({"name": "CanSave", "type": "bool"})
    refuses(candidate)
    for constructor in (None, {}, {"parameters": {}}, {"parameters": [], "inactive": True},
                        {"parameters": [{"name": "initial", "type": "Unknown"}]},
                        {"parameters": [{"name": "initial", "type": "void"}]},
                        {"parameters": [{"name": "initial;attack", "type": "Values"}]},
                        {"parameters": [{"name": "initial", "type": "Values"}, {"name": "initial", "type": "Values"}]}):
        candidate = copy.deepcopy(request)
        candidate["model_manifest"]["classes"][0]["constructor"] = constructor
        refuses(candidate)
    candidate = copy.deepcopy(request)
    del candidate["model_manifest"]["classes"][0]["constructor"]
    refuses(candidate)
    refuses(b'{"protocol":"x","protocol":"y"}')
    for count in (129,):
        candidate = copy.deepcopy(request)
        candidate["source"] = 'view Huge(model: FixtureModel) { Column(id:"root") {' + ''.join(
            f'Label(id:"n{index}");' for index in range(count)) + '} }'
        refuses(candidate)
    nested = 'Label(id:"leaf");'
    for index in range(9):
        nested = f'Column(id:"n{index}") {{ {nested} }}'
    candidate = copy.deepcopy(request)
    candidate["source"] = 'view Deep(model: FixtureModel) {' + nested + '}'
    refuses(candidate)
    print("native_model_view_protocol_refusals_and_generation=passed")


if __name__ == "__main__":
    main()
