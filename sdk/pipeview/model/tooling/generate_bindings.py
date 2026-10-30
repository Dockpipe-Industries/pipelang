#!/usr/bin/env python3
"""Compile native model/view bindings using one pinned exact installed model SDK."""
import argparse
import json
import math
import os
import re
from pathlib import Path
import signal
import subprocess
import tempfile
import threading

import sdk_manifest

MODEL_PROTOCOL = "pipelang.native-model.v1"
VIEW_PROTOCOL = "pipelang.native-model-view.v1"
STYLE_DESCRIPTOR_PROTOCOL = "pipelang.native-style-descriptor.v1"
MAXIMUM_SOURCE_BYTES = 65536
MAXIMUM_MESSAGE_BYTES = 1 << 20
MAXIMUM_DIAGNOSTIC_BYTES = 65536
TIMEOUT_SECONDS = 45
OUTPUT_NAMES = {"Model.hpp", "model.json", "ModelView.hpp", "view.json", "bindings.json"}


def text(value, label, maximum=65536):
    if not isinstance(value, str) or not value or len(value.encode("utf-8")) > maximum or "\0" in value:
        raise ValueError(label + " must be bounded nonempty Unicode text")
    return value


def object_array(value, label):
    if not isinstance(value, list) or len(value) > 128:
        raise ValueError(label + " must be a bounded array")
    return value


def named_entries(value, fields, label):
    names = set()
    for entry in object_array(value, label):
        sdk_manifest.exact_object(entry, fields, label)
        name = text(entry["name"], label + " name", 1024)
        if name in names:
            raise ValueError("Duplicate " + label + " name")
        names.add(name)
        yield entry


def typed_fields(value, label):
    for entry in named_entries(value, ("name", "type"), label):
        text(entry["type"], label + " type", 4096)


def methods(value, label):
    fields = ("name", "returnType", "parameters", "async", "pure")
    for method in named_entries(value, fields, label):
        text(method["returnType"], label + " return type", 4096)
        typed_fields(method["parameters"], label + " parameter")
        if type(method["async"]) is not bool or type(method["pure"]) is not bool:
            raise ValueError("Method async/pure metadata requires exact Booleans")


def admit_model(manifest, namespace):
    fields = ("protocol", "namespace", "records", "enums", "interfaces", "classes")
    sdk_manifest.exact_object(manifest, fields, "model manifest")
    if manifest["protocol"] != "pipelang.native-model-manifest.v1" or manifest["namespace"] != namespace:
        raise ValueError("Model manifest protocol or namespace mismatch")
    for record in named_entries(manifest["records"], ("name", "fields"), "record"):
        typed_fields(record["fields"], "record field")
    for enum in named_entries(manifest["enums"], ("name", "values"), "enum"):
        values = object_array(enum["values"], "enum values")
        for value in values:
            text(value, "enum member", 1024)
        if len(set(values)) != len(values):
            raise ValueError("Duplicate enum member")
    for interface in named_entries(manifest["interfaces"], ("name", "methods"), "interface"):
        methods(interface["methods"], "interface method")
    for model in named_entries(manifest["classes"], ("name", "fields", "methods", "constructor"), "class"):
        typed_fields(model["fields"], "class field")
        methods(model["methods"], "class method")
        sdk_manifest.exact_object(model["constructor"], ("parameters",), "constructor")
        typed_fields(model["constructor"]["parameters"], "constructor parameter")
    return manifest


def admit_view(manifest, namespace, model):
    sdk_manifest.exact_object(manifest, ("protocol", "name", "adapter", "model", "namespace"), "view manifest")
    if manifest["protocol"] != "pipelang.native-model-view-manifest.v1" or manifest["namespace"] != namespace:
        raise ValueError("View manifest protocol or namespace mismatch")
    for field in ("name", "adapter", "model"):
        text(manifest[field], "view " + field, 1024)
    if manifest["model"] not in {entry["name"] for entry in model["classes"]}:
        raise ValueError("View manifest refers to an unadmitted model")
    return manifest


def admit_style_descriptor(descriptor, view):
    sdk_manifest.exact_object(descriptor, ("protocol", "view", "style", "scope", "properties"),
                              "style descriptor")
    if descriptor["protocol"] != STYLE_DESCRIPTOR_PROTOCOL or descriptor["view"] != view["name"]:
        raise ValueError("Style descriptor protocol or view mismatch")
    if descriptor["scope"] != "resolved-authored":
        raise ValueError("Unsupported style descriptor scope")
    if descriptor["style"] is not None:
        text(descriptor["style"], "selected style")
    properties = descriptor["properties"]
    if not isinstance(properties, list) or len(properties) > 14:
        raise ValueError("Style descriptor properties require an array of at most 14 entries")
    if descriptor["style"] is None and properties:
        raise ValueError("An unselected style cannot have authored properties")
    lengths = {"padding": (0, 64), "gap": (0, 64), "textInset": (0, 64),
               "labelHeight": (1, 128), "controlHeight": (1, 256),
               "fontPixelHeight": (7, 56), "borderWidth": (0, 8)}
    colors = {"background", "control", "field", "border", "focus", "text", "disabledText"}
    previous = None
    for entry in properties:
        if not isinstance(entry, dict):
            raise ValueError("Style property must be an object")
        name = text(entry.get("name"), "style property name", 1024)
        if previous is not None and name <= previous:
            raise ValueError("Style properties must be unique and sorted by name")
        previous = name
        if name in lengths:
            sdk_manifest.exact_object(entry, ("name", "type", "unit", "value"), "length property")
            if entry["type"] != "length" or entry["unit"] != "px":
                raise ValueError("Style length requires length type and logical px units")
            value = entry["value"]
            minimum, maximum = lengths[name]
            valid_number = type(value) in (int, float) and minimum <= value <= maximum
            if not valid_number or not math.isfinite(value):
                raise ValueError("Style length exceeds its finite typed profile")
        elif name in colors:
            sdk_manifest.exact_object(entry, ("name", "type", "value"), "color property")
            value = entry["value"]
            canonical_color = isinstance(value, str) and re.fullmatch(r"#[0-9A-F]{8}", value)
            if entry["type"] != "color" or not canonical_color:
                raise ValueError("Style color requires canonical uppercase #RRGGBBAA")
        else:
            raise ValueError("Unsupported style descriptor property")
    return descriptor


def stop_process(process):
    try:
        if os.name == "posix":
            os.killpg(process.pid, signal.SIGKILL)
        else:
            process.kill()
    except ProcessLookupError:
        pass


def run_tool(tool, request):
    payload = json.dumps(request, ensure_ascii=False, allow_nan=False, separators=(",", ":")).encode()
    if len(payload) > MAXIMUM_MESSAGE_BYTES:
        raise ValueError("Native request exceeds its capacity")
    process = subprocess.Popen([str(tool)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, start_new_session=os.name == "posix")
    output = bytearray()
    diagnostic = bytearray()
    exceeded = threading.Event()

    def read(stream, destination, maximum):
        try:
            while block := stream.read(8192):
                if len(destination) + len(block) > maximum:
                    exceeded.set()
                    stop_process(process)
                    return
                destination.extend(block)
        finally:
            stream.close()

    def write():
        try:
            process.stdin.write(payload)
            process.stdin.close()
        except (BrokenPipeError, OSError):
            pass

    readers = [threading.Thread(target=read, args=(process.stdout, output, MAXIMUM_MESSAGE_BYTES), daemon=True),
               threading.Thread(target=read, args=(process.stderr, diagnostic, MAXIMUM_DIAGNOSTIC_BYTES), daemon=True)]
    writer = threading.Thread(target=write, daemon=True)
    for worker in readers + [writer]:
        worker.start()
    try:
        process.wait(timeout=TIMEOUT_SECONDS)
        for worker in readers + [writer]:
            worker.join(timeout=1)
        if any(worker.is_alive() for worker in readers + [writer]):
            raise ValueError("Native tool did not close its protocol streams")
        if exceeded.is_set():
            raise ValueError("Native response exceeds its capacity")
        if process.returncode != 0:
            raise ValueError("Native compiler process failed: " + diagnostic.decode("utf-8", errors="replace"))
        if diagnostic:
            raise ValueError("Native compiler emitted unexpected process diagnostics")
        return sdk_manifest.load_json(bytes(output))
    finally:
        if process.poll() is None or any(worker.is_alive() for worker in readers + [writer]):
            stop_process(process)
        process.wait(timeout=5)


def admit_response(response, protocol, style_inspection=False):
    if not isinstance(response, dict) or response.get("protocol") != protocol or type(response.get("ok")) is not bool:
        raise ValueError("Unexpected native response protocol or outcome")
    if not response["ok"]:
        sdk_manifest.exact_object(response, ("protocol", "ok", "diagnostics"), "native refusal")
        raise ValueError("Native compiler refused source: " + json.dumps(response["diagnostics"], ensure_ascii=False))
    fields = ("protocol", "ok", "header", "manifest")
    if style_inspection:
        fields += ("style_descriptor",)
    sdk_manifest.exact_object(response, fields, "native response")
    text(response["header"], "generated native header", MAXIMUM_MESSAGE_BYTES)
    return response["header"].encode("utf-8"), response["manifest"]


def read_source(path):
    if not path.is_absolute() or path.is_symlink() or not path.is_file():
        raise ValueError("Sources must be absolute regular files")
    with path.open("rb") as source:
        data = source.read(MAXIMUM_SOURCE_BYTES + 1)
    if not data or len(data) > MAXIMUM_SOURCE_BYTES:
        raise ValueError("Source must contain 1..65536 bytes")
    return data, data.decode("utf-8")


def canonical(value):
    return (json.dumps(value, sort_keys=True, ensure_ascii=False, allow_nan=False, indent=2) + "\n").encode()


def output_preflight(output):
    if not output.is_absolute() or output.is_symlink():
        raise ValueError("Output must be an absolute owned directory")
    if output.exists():
        if not output.is_dir():
            raise ValueError("Output is not a directory")
        for entry in output.iterdir():
            if entry.name not in OUTPUT_NAMES | {"style.json"} or entry.is_symlink() or not entry.is_file():
                raise ValueError("Output directory contains unowned material")


def publish(output, files):
    output_preflight(output)
    ancestor = output.parent
    while not ancestor.exists():
        ancestor = ancestor.parent
    with tempfile.TemporaryDirectory(prefix=".pipelang-model-", dir=ancestor) as directory:
        stage = Path(directory) / "bindings"
        stage.mkdir()
        for name, data in files.items():
            (stage / name).write_bytes(data)
        output.parent.mkdir(parents=True, exist_ok=True)
        previous = Path(directory) / "previous"
        existed = output.exists()
        if existed:
            os.replace(output, previous)
        try:
            os.replace(stage, output)
        except OSError:
            if existed:
                os.replace(previous, output)
            raise


def generate(prefix, manifest_path, manifest_hash, model_tool, view_tool, model_path, view_path,
             namespace, output, style_inspection=False):
    if type(style_inspection) is not bool:
        raise ValueError("Style inspection selection requires an exact Boolean")
    text(namespace, "namespace", 1024)
    output_preflight(output)
    for source in (model_path, view_path):
        if source == output or output in source.parents:
            raise ValueError("Output directory cannot own an input source")
    manifest = sdk_manifest.verify(prefix, manifest_path, manifest_hash)
    files = {entry["path"]: entry for entry in manifest["files"]}
    tool_hashes = {}
    for name, tool in (("model", model_tool), ("view", view_tool)):
        if not tool.is_absolute() or tool.is_symlink() or not tool.is_file() or not os.access(tool, os.X_OK):
            raise ValueError("Native tool must be an installed absolute executable")
        relative = tool.relative_to(prefix).as_posix()
        if relative not in files:
            raise ValueError("Native tool is absent from the pinned SDK manifest")
        tool_hashes[name] = sdk_manifest.file_hash(tool)
        if tool_hashes[name] != files[relative]["sha256"]:
            raise ValueError("Native tool checksum mismatch")
    model_bytes, model_source = read_source(model_path)
    view_bytes, view_source = read_source(view_path)
    model_response = run_tool(model_tool, {"protocol": MODEL_PROTOCOL, "source": model_source, "namespace": namespace})
    model_header, model_manifest = admit_response(model_response, MODEL_PROTOCOL)
    admit_model(model_manifest, namespace)
    view_request = {"protocol": VIEW_PROTOCOL, "source": view_source,
                    "model_manifest": model_manifest, "namespace": namespace,
                    "model_header": "Model.hpp"}
    if style_inspection:
        view_request["style_inspection"] = STYLE_DESCRIPTOR_PROTOCOL
    view_response = run_tool(view_tool, view_request)
    view_header, view_manifest = admit_response(view_response, VIEW_PROTOCOL, style_inspection)
    admit_view(view_manifest, namespace, model_manifest)
    if style_inspection:
        admit_style_descriptor(view_response["style_descriptor"], view_manifest)
    if read_source(model_path)[0] != model_bytes or read_source(view_path)[0] != view_bytes:
        raise ValueError("Source changed during native compilation")
    for name, tool in (("model", model_tool), ("view", view_tool)):
        if sdk_manifest.file_hash(tool) != tool_hashes[name]:
            raise ValueError("Native executable changed during compilation")
    sdk_manifest.verify(prefix, manifest_path, manifest_hash)
    outputs = {"Model.hpp": model_header, "model.json": canonical(model_manifest),
               "ModelView.hpp": view_header, "view.json": canonical(view_manifest)}
    if style_inspection:
        outputs["style.json"] = canonical(view_response["style_descriptor"])
    receipt = {"protocol": "pipelang.native-model-bindings.v1", "sdk_version": "0.1.0",
               "sdk_manifest_sha256": manifest_hash, "namespace": namespace,
               "model_source_sha256": sdk_manifest.sha256(model_bytes),
               "view_source_sha256": sdk_manifest.sha256(view_bytes),
               "tools": {"model": {"protocol": MODEL_PROTOCOL, "sha256": tool_hashes["model"]},
                         "view": {"protocol": VIEW_PROTOCOL, "sha256": tool_hashes["view"]}},
               "generated": {name: sdk_manifest.sha256(data) for name, data in outputs.items()}}
    outputs["bindings.json"] = canonical(receipt)
    publish(output, outputs)
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("sdk-prefix", "sdk-manifest", "model-compiler", "view-compiler", "model", "view", "output-directory"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--sdk-manifest-sha256", required=True)
    parser.add_argument("--namespace", required=True)
    parser.add_argument("--style-inspection", action="store_true")
    args = parser.parse_args()
    generate(args.sdk_prefix, args.sdk_manifest, args.sdk_manifest_sha256,
             args.model_compiler, args.view_compiler, args.model, args.view,
             args.namespace, args.output_directory, style_inspection=args.style_inspection)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        raise SystemExit("pipelang-model-bindings: " + str(error))
