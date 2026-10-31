#!/usr/bin/env python3
"""Exercise optional style sidecars through the actual installed SDK generator."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

from test_styles import BASE


STYLE_PROTOCOL = "pipelang.native-style-descriptor.v1"
CORE_NAMES = {"Model.hpp", "model.json", "ModelView.hpp", "view.json"}
DEFAULT_NAMES = CORE_NAMES | {"bindings.json"}


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def snapshot(directory):
    files = {}
    for path in directory.iterdir():
        require(path.is_file() and not path.is_symlink(), "Unexpected generated directory entry: " + str(path))
        files[path.name] = path.read_bytes()
    return files


def check_outputs(directory, selected, core_baseline=None):
    files = snapshot(directory)
    expected_names = DEFAULT_NAMES | {"style.json"} if selected else DEFAULT_NAMES
    require(set(files) == expected_names, "Generator output ownership differs from the selected mode")
    receipt = json.loads(files["bindings.json"])
    generated_names = CORE_NAMES | {"style.json"} if selected else CORE_NAMES
    require(set(receipt["generated"]) == generated_names, "Receipt omits or adds generated artifacts")
    for name in sorted(generated_names):
        require(receipt["generated"][name] == digest(files[name]), "Receipt checksum mismatch: " + name)
    core = {}
    for name in sorted(CORE_NAMES):
        core[name] = files[name]
    if core_baseline is not None:
        require(core == core_baseline, "Style inspection changed a core generated artifact")
    if selected:
        descriptor = json.loads(files["style.json"])
        expected_descriptor = {
            "protocol": STYLE_PROTOCOL,
            "view": "FixtureView",
            "style": "Compact",
            "scope": "resolved-authored",
            "properties": [
                {"name": "background", "type": "color", "value": "#102030FF"},
                {"name": "controlHeight", "type": "length", "unit": "px", "value": 42},
                {"name": "disabledText", "type": "color", "value": "#667788FF"},
                {"name": "focus", "type": "color", "value": "#AABBCCFF"},
                {"name": "gap", "type": "length", "unit": "px", "value": 4},
                {"name": "padding", "type": "length", "unit": "px", "value": 10.5},
                {"name": "text", "type": "color", "value": "#DDEEFFFF"},
            ],
        }
        require(descriptor == expected_descriptor, "Installed compiler style descriptor differs from the authored oracle")
    return core


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-prefix", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    prefix = args.sdk_prefix
    output = args.output
    require(prefix.is_absolute() and prefix.is_dir() and not prefix.is_symlink(),
            "SDK prefix must be an absolute installed directory")
    require(output.is_absolute() and not output.exists() and not output.is_symlink(),
            "Output must be a fresh absolute owned directory")
    fixture = Path(__file__).resolve().parent
    manifest = prefix / "share/pipelang-model/sdk-manifest.json"
    generator = prefix / "share/pipelang-model/tooling/generate_bindings.py"
    require(manifest.is_file() and generator.is_file(), "Installed SDK generator or manifest is missing")
    output.mkdir(parents=True)
    view_source = (fixture / "Fixture.pipe").read_text()
    declaration = "view FixtureView(model: FixtureModel) {"
    require(view_source.count(declaration) == 1, "Fixture view declaration has changed")
    styled_source = BASE + view_source.replace(declaration, "view FixtureView(model: FixtureModel) style Compact {")
    styled_view = output / "Styled.pipe"
    styled_view.write_text(styled_source)
    bindings_output = output / "bindings"
    command = [
        sys.executable, "-B", str(generator),
        "--sdk-prefix", str(prefix),
        "--sdk-manifest", str(manifest),
        "--sdk-manifest-sha256", digest(manifest.read_bytes()),
        "--model-compiler", str(prefix / "bin/pipelang-modelc"),
        "--view-compiler", str(prefix / "bin/pipelang-model-viewc"),
        "--model", str(fixture / "FixtureModel.pipe"),
        "--view", str(styled_view),
        "--namespace", "pipelang_models",
        "--output-directory", str(bindings_output),
    ]

    def generate(selected, view=styled_view, refusal=False):
        invocation = list(command)
        invocation[invocation.index("--view") + 1] = str(view)
        if selected:
            invocation.append("--style-inspection")
        result = subprocess.run(invocation, capture_output=True, text=True, timeout=60)
        if refusal:
            require(result.returncode != 0, "Installed generator unexpectedly admitted a protected candidate")
            require(bool(result.stderr), "Expected generator refusal diagnostics are missing")
        else:
            require(result.returncode == 0, "Installed generation failed: " + result.stderr)
        return result

    generate(False)
    core_baseline = check_outputs(bindings_output, False)
    modes = ["default"]
    for selected, mode in ((True, "enabled"), (False, "disabled"), (True, "enabled")):
        generate(selected)
        check_outputs(bindings_output, selected, core_baseline)
        modes.append(mode)

    invalid_view = output / "Invalid.pipe"
    invalid_source = styled_source.replace("override gap: 4px;", "override gap: 99px;")
    require(invalid_source != styled_source, "Invalid fixture did not change the authored gap")
    invalid_view.write_text(invalid_source)
    protected = snapshot(bindings_output)
    invalid_result = generate(True, view=invalid_view, refusal=True)
    require("refused source" in invalid_result.stderr, "Invalid style did not reach compiler admission")
    require(snapshot(bindings_output) == protected, "Invalid style changed previous complete bindings")

    user_file = bindings_output / "user.json"
    user_file.write_text('{"protected":"user material"}\n')
    protected_with_user = snapshot(bindings_output)
    unknown_result = generate(True, refusal=True)
    require("unowned material" in unknown_result.stderr, "Unknown file did not trigger output ownership refusal")
    require(snapshot(bindings_output) == protected_with_user, "Ownership refusal changed protected output")
    project = output / "cmake-project"
    project.mkdir()
    (project / "CMakeLists.txt").write_text("""cmake_minimum_required(VERSION 3.21.1)
project(StyleInspectionGeneration LANGUAGES CXX)
find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED)
option(INSPECT_STYLE "Generate the authored style descriptor" OFF)
set(inspection_arguments)
if(INSPECT_STYLE)
    list(APPEND inspection_arguments STYLE_INSPECTION)
endif()
pipelang_add_model_bindings(fixture
    ${inspection_arguments}
    MODEL "${FIXTURE_MODEL}"
    VIEW "${FIXTURE_VIEW}"
    NAMESPACE pipelang_models
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bindings")
""")
    build = output / "cmake-build"
    cmake_modes = []
    for selected, mode in ((False, "disabled"), (True, "enabled"), (False, "disabled")):
        configure = ["cmake", "-S", str(project), "-B", str(build),
                     "-DPipeLangModel_DIR=" + str(prefix / "lib/cmake/PipeLangModel"),
                     "-DFIXTURE_MODEL=" + str(fixture / "FixtureModel.pipe"),
                     "-DFIXTURE_VIEW=" + str(styled_view),
                     "-DINSPECT_STYLE=" + ("ON" if selected else "OFF")]
        commands = [configure, ["cmake", "--build", str(build), "--target", "fixture_generate", "--parallel", "1"]]
        for command in commands:
            result = subprocess.run(command, capture_output=True, text=True, timeout=60)
            require(result.returncode == 0, "Installed CMake generation failed: " + result.stderr + result.stdout)
        check_outputs(build / "bindings", selected, core_baseline)
        cmake_modes.append(mode)
    print(json.dumps({"status": "passed", "mode_sequences": modes,
                      "cmake_modes": cmake_modes, "refusal_count": 2}, sort_keys=True))


if __name__ == "__main__":
    main()
