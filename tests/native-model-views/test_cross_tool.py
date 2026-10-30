#!/usr/bin/env python3
"""Use real modelc manifests as the model-viewc schema interoperability oracle."""
import argparse
import copy
import json
from pathlib import Path
import subprocess


def invoke(compiler, request):
    process = subprocess.run([str(compiler)], input=json.dumps(request).encode(),
                             capture_output=True, timeout=5, check=True)
    return json.loads(process.stdout)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--model-compiler", type=Path, required=True)
    parser.add_argument("--view-compiler", type=Path, required=True)
    args = parser.parse_args()
    fixtures = Path(__file__).resolve().parent
    model_source = (fixtures / "FixtureModel.pipe").read_text()
    view_source = (fixtures / "Fixture.pipe").read_text()
    for source, parameters in [(model_source, [{"name": "initial", "type": "Values"}]),
                               (model_source.replace("public FixtureModel(Values initial) {\n        Draft = initial;\n    }", ""), [])]:
        model = invoke(args.model_compiler, {
            "protocol": "pipelang.native-model.v1",
            "namespace": "pipelang_models",
            "source": source,
        })
        assert model["ok"], model
        manifest = model["manifest"]
        assert manifest["classes"][0]["constructor"] == {"parameters": parameters}
        request = {
            "protocol": "pipelang.native-model-view.v1",
            "namespace": "pipelang_models",
            "source": view_source,
            "model_header": "Model.hpp",
            "model_manifest": manifest,
        }
        view = invoke(args.view_compiler, request)
        assert view["ok"], view
        assert view["manifest"]["adapter"] == "FixtureViewAdapter"
        for malformed in [None, {"parameters": [], "unexpected": True}]:
            invalid = copy.deepcopy(request)
            invalid["model_manifest"]["classes"][0]["constructor"] = malformed
            refused = invoke(args.view_compiler, invalid)
            assert refused["ok"] is False and "header" not in refused
    print("native_model_to_view_manifest_interoperability=passed")


if __name__ == "__main__":
    main()
