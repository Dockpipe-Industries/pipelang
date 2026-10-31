"""Focused raw-process checks for the admitted-Core native emitter boundary.

Run this module inside tests/containedexec/run.py. Complete Core semantic
validation remains with the Go caller; these tests exercise framing, structure,
closed backend capabilities and reference-export byte compatibility.
"""

import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import unittest


REQUEST_CONTRACT = "pipelang.cpp-emitter.request.v1"
RESPONSE_CONTRACT = "pipelang.cpp-emitter.response.v1"
MAXIMUM_INPUT_BYTES = 16 * 1024 * 1024
MAXIMUM_RESPONSE_BYTES = 32 * 1024 * 1024


class EmitterProtocolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        emitter_value = os.environ.get("PIPELANG_CPP_EMITTER")
        if not emitter_value:
            raise unittest.SkipTest("native emitter is opt-in")

        cls.emitter = Path(emitter_value)
        if not cls.emitter.is_absolute() or not cls.emitter.is_file():
            raise AssertionError("PIPELANG_CPP_EMITTER must identify an existing absolute executable")
        if not os.access(cls.emitter, os.X_OK):
            raise AssertionError("PIPELANG_CPP_EMITTER must be executable")

        corpus_value = os.environ.get("PIPELANG_CPP_EMITTER_CORPUS")
        if not corpus_value:
            raise AssertionError("PIPELANG_CPP_EMITTER_CORPUS is required with the native emitter")
        cls.corpus = Path(corpus_value)
        if not cls.corpus.is_absolute() or not cls.corpus.is_dir():
            raise AssertionError("PIPELANG_CPP_EMITTER_CORPUS must identify an existing absolute directory")

        cls.scalar_program = cls.read_program("N1")

    @classmethod
    def read_program(cls, case):
        path = cls.corpus / case / "core.json"
        if not path.is_file():
            raise AssertionError("missing reference Core fixture: " + str(path))
        return json.loads(path.read_text(encoding="utf-8"))

    def invoke(self, payload):
        process = subprocess.run(
            [str(self.emitter)],
            input=payload,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=5,
            check=False,
        )
        self.assertLessEqual(len(process.stdout), MAXIMUM_RESPONSE_BYTES)
        return process

    @staticmethod
    def request(program):
        return {
            "contract": REQUEST_CONTRACT,
            "program": program,
        }

    def invoke_request(self, request):
        payload = json.dumps(request, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        return self.invoke(payload)

    def assert_refused(self, payload):
        process = self.invoke(payload)
        self.assertNotEqual(process.returncode, 0)
        self.assertEqual(process.stdout, b"", "a rejected request must not publish partial output")
        self.assertTrue(process.stderr, "a rejected request must explain the refusal on stderr")

    def assert_program_refused(self, program):
        payload = json.dumps(self.request(program), separators=(",", ":")).encode("utf-8")
        self.assert_refused(payload)

    def fresh_program(self):
        return copy.deepcopy(self.scalar_program)

    def test_reference_export_source_and_bindings(self):
        for case in ("N1", "A1", "A2"):
            with self.subTest(case=case):
                program = self.read_program(case)
                process = self.invoke_request(self.request(program))
                self.assertEqual(process.returncode, 0, process.stderr.decode("utf-8", errors="replace"))
                self.assertEqual(process.stderr, b"")
                response = json.loads(process.stdout)
                self.assertEqual(response["contract"], RESPONSE_CONTRACT)
                expected_source = (self.corpus / case / "generated.hpp").read_bytes()
                self.assertEqual(response["source"].encode("utf-8"), expected_source)

                def key(function):
                    identity = function["identity"]
                    return (identity["package_id"] + "\0" + identity["path"]).encode("utf-8")

                expected_bindings = []
                for function in sorted(program["functions"], key=key):
                    digest = hashlib.sha256(key(function)).hexdigest()[:24]
                    expected_bindings.append({
                        "identity": function["identity"],
                        "name": "fn_" + digest,
                    })
                self.assertEqual(response["functions"], expected_bindings)

    def test_malformed_json_and_trailing_documents(self):
        valid = json.dumps(self.request(self.scalar_program)).encode("utf-8")
        for payload in (b"", b"{", b"null", b"[]", valid + b" {}", valid + b" garbage"):
            with self.subTest(prefix=payload[:24]):
                self.assert_refused(payload)

    def test_duplicate_keys_at_root_and_nested_object(self):
        program = json.dumps(self.scalar_program, separators=(",", ":")).encode("utf-8")
        duplicate_root = (
            b'{"contract":"pipelang.cpp-emitter.request.v1",'
            b'"contract":"pipelang.cpp-emitter.request.v1","program":' + program + b"}"
        )
        duplicate_nested = (
            b'{"contract":"pipelang.cpp-emitter.request.v1","program":' + program
            + b',"extra":{"name":1,"na\\u006de":2}}'
        )
        self.assert_refused(duplicate_root)
        self.assert_refused(duplicate_nested)

    def test_closed_request_and_core_contracts(self):
        request = self.request(self.fresh_program())
        request["contract"] = "pipelang.cpp-emitter.request.future"
        self.assert_refused(json.dumps(request).encode("utf-8"))
        for field in ("language_contract", "compiler_contract"):
            with self.subTest(field=field):
                program = self.fresh_program()
                program[field] = "unsupported"
                self.assert_program_refused(program)

    def test_closed_expression_and_type_kinds(self):
        program = self.fresh_program()
        program["functions"][0]["body"]["kind"] = "unsupported_expression"
        self.assert_program_refused(program)
        program = self.fresh_program()
        program["functions"][0]["body"]["type"] = {"kind": "unsupported_type"}
        self.assert_program_refused(program)

    def test_missing_required_members(self):
        for member in ("body", "return_type", "identity"):
            with self.subTest(member=member):
                program = self.fresh_program()
                del program["functions"][0][member]
                self.assert_program_refused(program)
        request = {"contract": REQUEST_CONTRACT}
        self.assert_refused(json.dumps(request).encode("utf-8"))

    def test_missing_and_negative_references(self):
        for position in (-1, 1000000):
            with self.subTest(position=position):
                program = self.fresh_program()
                original_type = program["functions"][0]["body"]["type"]
                program["functions"][0]["body"] = {
                    "kind": "reference",
                    "type": original_type,
                    "parameter": position,
                }
                self.assert_program_refused(program)

    def test_invalid_and_duplicate_function_identities(self):
        for field in ("package_id", "path"):
            for value in ("", -1):
                with self.subTest(field=field, value=value):
                    program = self.fresh_program()
                    program["functions"][0]["identity"][field] = value
                    self.assert_program_refused(program)
        program = self.fresh_program()
        program["functions"].append(copy.deepcopy(program["functions"][0]))
        self.assert_program_refused(program)

    def test_noncanonical_parameter_positions(self):
        for position in (-1, 1):
            with self.subTest(position=position):
                program = self.fresh_program()
                program["functions"][0]["parameters"] = [{
                    "position": position,
                    "name": "parameter",
                    "type": {"kind": "primitive", "primitive": "bool"},
                }]
                self.assert_program_refused(program)

    def test_input_byte_limit(self):
        # A whitespace-only request bounds allocation without generating output.
        self.assert_refused(b" " * (MAXIMUM_INPUT_BYTES + 1))

    def test_input_depth_limit(self):
        self.assert_refused(b"[" * 514 + b"0" + b"]" * 514)

    def test_input_node_limit(self):
        self.assert_refused(b"[" + b"null," * 100000 + b"null]")


if __name__ == "__main__":
    unittest.main()
