#!/usr/bin/env python3
"""Independent native style descriptor and exact default-wire compatibility oracle."""
import argparse
import copy
import json
from pathlib import Path
import subprocess

from test_protocol import manifest
from test_styles import BASE


PROTOCOL = "pipelang.native-style-descriptor.v1"


def invoke(compiler, request):
    process = subprocess.run([str(compiler)], input=json.dumps(request).encode(),
                             capture_output=True, timeout=5, check=True)
    assert not process.stderr, process.stderr
    return process.stdout, json.loads(process.stdout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--baseline-compiler", type=Path)
    args = parser.parse_args()
    source = (Path(__file__).resolve().parent / "Fixture.pipe").read_text()
    request = {"protocol": "pipelang.native-model-view.v1", "source": source,
               "model_manifest": manifest(), "namespace": "pipelang_models", "model_header": "Model.hpp"}
    styled = copy.deepcopy(request)
    styled["source"] = BASE + source.replace("FixtureModel) {", "FixtureModel) style Compact {")
    defaults = []
    for original in (request, styled):
        wire, response = invoke(args.compiler, original)
        assert response["ok"] and "style_descriptor" not in response, response
        if args.baseline_compiler:
            baseline_wire, _ = invoke(args.baseline_compiler, original)
            assert wire == baseline_wire, "default protocol bytes changed"
        selected = dict(original, style_inspection=PROTOCOL)
        _, inspected = invoke(args.compiler, selected)
        assert inspected["ok"], inspected
        descriptor = inspected.pop("style_descriptor")
        assert inspected == response, "inspection changed generated header or manifest"
        assert set(descriptor) == {"protocol", "view", "style", "scope", "properties"}
        assert descriptor["protocol"] == PROTOCOL and descriptor["view"] == "FixtureView"
        assert descriptor["scope"] == "resolved-authored"
        defaults.append(descriptor)
    assert defaults[0]["style"] is None and defaults[0]["properties"] == [], defaults[0]
    expected = [
        {"name": "background", "type": "color", "value": "#102030FF"},
        {"name": "controlHeight", "type": "length", "unit": "px", "value": 42},
        {"name": "disabledText", "type": "color", "value": "#667788FF"},
        {"name": "focus", "type": "color", "value": "#AABBCCFF"},
        {"name": "gap", "type": "length", "unit": "px", "value": 4},
        {"name": "padding", "type": "length", "unit": "px", "value": 10.5},
        {"name": "text", "type": "color", "value": "#DDEEFFFF"},
    ]
    assert defaults[1]["style"] == "Compact" and defaults[1]["properties"] == expected, defaults[1]
    serialized = json.dumps(defaults[1])
    for excluded in ("inset", "ink", "scale", "model.Draft", "CanSave", "Before"):
        assert excluded not in serialized, excluded
    unselected = dict(request, source=BASE + source, style_inspection=PROTOCOL)
    _, response = invoke(args.compiler, unselected)
    assert response["style_descriptor"] == defaults[0], "unselected theme leaked metadata"
    empty = dict(request, source="style Empty {}\n" + source.replace("FixtureModel) {", "FixtureModel) style Empty {"),
                 style_inspection=PROTOCOL)
    _, response = invoke(args.compiler, empty)
    assert response["style_descriptor"]["style"] == "Empty" and response["style_descriptor"]["properties"] == []
    refusals = 0
    for selection in (False, True, None, 1, [], {}, "", "pipelang.native-style-descriptor.v2"):
        _, response = invoke(args.compiler, dict(styled, style_inspection=selection))
        assert not response["ok"] and set(response) == {"protocol", "ok", "diagnostics"}, response
        refusals += 1
    invalid = dict(styled, style_inspection=PROTOCOL)
    invalid["source"] = invalid["source"].replace("gap: 8px", "gap: 99px")
    _, response = invoke(args.compiler, invalid)
    assert not response["ok"] and "style_descriptor" not in response and "header" not in response
    refusals += 1
    print(json.dumps({"status": "passed", "inspection_refusals": refusals,
                      "default_wire_parity": bool(args.baseline_compiler)}))


if __name__ == "__main__":
    main()
