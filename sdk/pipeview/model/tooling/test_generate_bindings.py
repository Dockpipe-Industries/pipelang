#!/usr/bin/env python3
"""Admission and publication fixtures; no compiler/source implementation lives here."""
import copy
import json
import os
import subprocess
import sys
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import generate_bindings as bindings
import sdk_manifest


class BindingsAdmissionTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.prefix = self.root / "sdk"
        self.prefix.mkdir()
        self.model_tool = self.prefix / "bin/pipelang-modelc"
        self.view_tool = self.prefix / "bin/pipelang-model-viewc"
        for tool in (self.model_tool, self.view_tool):
            tool.parent.mkdir(exist_ok=True)
            tool.write_text("#!/bin/sh\nexit 0\n")
            tool.chmod(0o700)
        runtime = self.prefix / "include/pipelang/model/runtime.hpp"
        runtime.parent.mkdir(parents=True)
        runtime.write_text("// fixture runtime\n")
        self.sdk_manifest_path = self.prefix / "sdk-manifest.json"
        manifest = {"protocol": sdk_manifest.PROTOCOL, "version": sdk_manifest.VERSION,
                    "roots": ["bin", "include"],
                    "files": sdk_manifest.inventory(self.prefix, ["bin", "include"], "sdk-manifest.json")}
        self.sdk_manifest_path.write_bytes(bindings.canonical(manifest))
        self.manifest_hash = sdk_manifest.file_hash(self.sdk_manifest_path)
        self.model_path = self.root / "Model.pipe"
        self.view_path = self.root / "View.pipe"
        self.model_path.write_text("opaque native model source")
        self.view_path.write_text("opaque native view source")
        self.output = self.root / "generated"
        self.model_manifest = {"protocol": "pipelang.native-model-manifest.v1", "namespace": "fixture",
                               "records": [], "enums": [], "interfaces": [],
                               "classes": [{"name": "Model", "fields": [], "methods": [],
                                            "constructor": {"parameters": []}}]}
        self.view_manifest = {"protocol": "pipelang.native-model-view-manifest.v1", "namespace": "fixture",
                              "name": "View", "adapter": "ViewAdapter", "model": "Model"}
        self.responses = [
            {"protocol": bindings.MODEL_PROTOCOL, "ok": True, "header": "// native model\n", "manifest": self.model_manifest},
            {"protocol": bindings.VIEW_PROTOCOL, "ok": True, "header": "// native view\n", "manifest": self.view_manifest},
        ]

    def generate(self):
        return bindings.generate(self.prefix, self.sdk_manifest_path, self.manifest_hash,
                                 self.model_tool, self.view_tool, self.model_path, self.view_path,
                                 "fixture", self.output)

    def seed(self):
        self.output.mkdir()
        for name in bindings.OUTPUT_NAMES:
            (self.output / name).write_text("sentinel " + name)

    def snapshot(self):
        if not self.output.exists():
            return None
        return {path.name: path.read_bytes() for path in self.output.iterdir()}

    def test_complete_binding_receipt_and_installed_runtime_identity(self):
        with mock.patch.object(bindings, "run_tool", side_effect=self.responses) as compiler:
            receipt = self.generate()
        self.assertEqual(set(self.snapshot()), bindings.OUTPUT_NAMES)
        self.assertEqual(receipt["sdk_manifest_sha256"], self.manifest_hash)
        self.assertEqual(receipt["tools"]["model"]["sha256"], sdk_manifest.file_hash(self.model_tool))
        self.assertEqual(receipt["tools"]["view"]["sha256"], sdk_manifest.file_hash(self.view_tool))
        for name, expected_hash in receipt["generated"].items():
            self.assertEqual(sdk_manifest.file_hash(self.output / name), expected_hash)
        self.assertEqual(compiler.call_args_list[1].args[1]["model_manifest"], self.model_manifest)
        self.assertEqual(compiler.call_args_list[1].args[1]["model_header"], "Model.hpp")

    def test_refusals_leave_absent_and_existing_outputs_unchanged(self):
        for existing in (False, True):
            for failure in ("model refusal", "view refusal", "view protocol", "view identity", "extra response"):
                with self.subTest(existing=existing, failure=failure):
                    if self.output.exists():
                        for path in self.output.iterdir():
                            path.unlink()
                        self.output.rmdir()
                    if existing:
                        self.seed()
                    before = self.snapshot()
                    responses = copy.deepcopy(self.responses)
                    if failure == "model refusal":
                        responses[0] = {"protocol": bindings.MODEL_PROTOCOL, "ok": False, "diagnostics": []}
                    elif failure == "view refusal":
                        responses[1] = {"protocol": bindings.VIEW_PROTOCOL, "ok": False, "diagnostics": []}
                    elif failure == "view protocol":
                        responses[1]["protocol"] = bindings.MODEL_PROTOCOL
                    elif failure == "view identity":
                        responses[1]["manifest"]["model"] = "UnknownModel"
                    else:
                        responses[1]["extra"] = False
                    with mock.patch.object(bindings, "run_tool", side_effect=responses):
                        with self.assertRaises(ValueError):
                            self.generate()
                    self.assertEqual(self.snapshot(), before)

    def test_changes_during_compilation_refuse_without_publication(self):
        for changed in ("model source", "view source", "tool", "runtime", "manifest"):
            with self.subTest(changed=changed):
                calls = 0
                originals = {path: path.read_bytes() for path in (
                    self.model_path, self.view_path, self.model_tool,
                    self.prefix / "include/pipelang/model/runtime.hpp", self.sdk_manifest_path)}
                def compile_tool(tool, request):
                    nonlocal calls
                    response = self.responses[calls]
                    calls += 1
                    if calls == 2:
                        path = {"model source": self.model_path, "view source": self.view_path,
                                "tool": self.model_tool, "runtime": self.prefix / "include/pipelang/model/runtime.hpp",
                                "manifest": self.sdk_manifest_path}[changed]
                        path.write_bytes(path.read_bytes() + b"\n")
                    return response
                with mock.patch.object(bindings, "run_tool", side_effect=compile_tool):
                    with self.assertRaises(ValueError):
                        self.generate()
                self.assertFalse(self.output.exists())
                for path, original in originals.items():
                    path.write_bytes(original)

    def test_sdk_mismatch_refuses_before_tool_execution(self):
        runtime = self.prefix / "include/pipelang/model/runtime.hpp"
        runtime.write_text("changed runtime")
        with mock.patch.object(bindings, "run_tool") as compiler:
            with self.assertRaisesRegex(ValueError, "SDK content"):
                self.generate()
        compiler.assert_not_called()
        self.assertFalse(self.output.exists())

    def test_publication_failure_restores_previous_complete_directory(self):
        self.seed()
        before = self.snapshot()
        replace = os.replace

        def fail_publication(source, target):
            if Path(source).name == "bindings":
                raise OSError("injected publication failure")
            return replace(source, target)

        with mock.patch.object(bindings, "run_tool", side_effect=self.responses):
            with mock.patch.object(bindings.os, "replace", side_effect=fail_publication):
                with self.assertRaisesRegex(OSError, "publication failure"):
                    self.generate()
        self.assertEqual(self.snapshot(), before)

    def test_native_process_capacity_timeout_and_duplicate_json_refusals(self):
        tool = self.root / "fixture-tool"
        scripts = {
            "capacity": "import sys\nsys.stdout.write('x' * ((1 << 20) + 1))\n",
            "timeout": "import time\ntime.sleep(5)\n",
            "duplicate": "print('{\"ok\":true,\"ok\":false}')\n",
        }
        for name, script in scripts.items():
            with self.subTest(name=name):
                tool.write_text("#!" + sys.executable + "\n" + script)
                tool.chmod(0o700)
                expected = subprocess.TimeoutExpired if name == "timeout" else ValueError
                timeout = 0.1 if name == "timeout" else 5
                with mock.patch.object(bindings, "TIMEOUT_SECONDS", timeout):
                    with self.assertRaises(expected):
                        bindings.run_tool(tool, {"protocol": bindings.MODEL_PROTOCOL})

    def test_strict_json_and_exact_metadata_shapes(self):
        for data in (b'{"ok":true,"ok":false}', b'{"value":NaN}', b'{"value":1}{}'):
            with self.assertRaises(ValueError):
                sdk_manifest.load_json(data)
        manifest = copy.deepcopy(self.model_manifest)
        manifest["classes"][0]["methods"] = [{"name": "Bad", "returnType": "void", "parameters": [],
                                               "async": 1, "pure": False}]
        with self.assertRaisesRegex(ValueError, "Booleans"):
            bindings.admit_model(manifest, "fixture")
        for value in ("A" * 64, "0" * 63, "0" * 65, False):
            with self.assertRaises(ValueError):
                sdk_manifest.exact_hash(value)


if __name__ == "__main__":
    unittest.main()
