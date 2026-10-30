"""Raw-process checks for the opt-in native lexer protocol.

Run this module inside tests/containedexec/run.py. It invokes only the lexer;
no generated program is compiled or executed. Go differential tests own the
complete token and diagnostic compatibility corpus.
"""

import json
import os
from pathlib import Path
import subprocess
import unittest


REQUEST_CONTRACT = "pipelang.lexer.request.v1"
RESPONSE_CONTRACT = "pipelang.lexer.response.v1"
MAXIMUM_INPUT_BYTES = 512 * 1024
MAXIMUM_SOURCE_BYTES = 65536
MAXIMUM_RESPONSE_BYTES = 16 * 1024 * 1024


class LexerProtocolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        lexer_value = os.environ.get("PIPELANG_CPP_LEXER")
        if not lexer_value:
            raise unittest.SkipTest("native lexer is opt-in")

        cls.lexer = Path(lexer_value)
        if not cls.lexer.is_absolute() or not cls.lexer.is_file():
            raise AssertionError("PIPELANG_CPP_LEXER must identify an existing absolute executable")
        if not os.access(cls.lexer, os.X_OK):
            raise AssertionError("PIPELANG_CPP_LEXER must be executable")

    @staticmethod
    def request(source):
        return {"contract": REQUEST_CONTRACT, "source": source}

    @staticmethod
    def encode(request):
        return json.dumps(request, ensure_ascii=False, separators=(",", ":")).encode("utf-8")

    def invoke(self, payload):
        process = subprocess.run(
            [str(self.lexer)],
            input=payload,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=5,
            check=False,
        )
        self.assertLessEqual(len(process.stdout), MAXIMUM_RESPONSE_BYTES)
        return process

    def assert_refused(self, payload):
        process = self.invoke(payload)
        self.assertEqual(process.returncode, 2)
        self.assertEqual(process.stdout, b"", "a rejected request must not publish partial output")
        self.assertTrue(process.stderr, "a rejected request must explain the refusal on stderr")
        return process

    def assert_response(self, source, expected):
        process = self.invoke(self.encode(self.request(source)))
        self.assertEqual(process.returncode, 0, process.stderr.decode("utf-8", errors="replace"))
        self.assertEqual(process.stderr, b"")
        self.assertEqual(json.loads(process.stdout), expected)

    def test_empty_source_has_terminal_eof(self):
        self.assert_response("", {
            "contract": RESPONSE_CONTRACT,
            "tokens": [{"kind": "eof", "literal": "", "start": 0, "end": 0}],
            "failure": None,
        })

    def test_lexical_failures_are_successful_protocol_responses(self):
        cases = (
            ("12.", "PL1002", "invalid float literal", 0, 3),
            ('"\\q"', "PL1003", "invalid escape sequence", 1, 3),
            ('"tail', "PL1004", "unterminated string literal", 0, 5),
            ("\x00", "PL1001", "unexpected character '\\x00'", 0, 1),
        )
        for source, code, message, start, end in cases:
            with self.subTest(source=source):
                self.assert_response(source, {
                    "contract": RESPONSE_CONTRACT,
                    "tokens": None,
                    "failure": {
                        "code": code,
                        "message": message,
                        "start": start,
                        "end": end,
                    },
                })

    def test_malformed_json_and_trailing_documents(self):
        valid = self.encode(self.request(""))
        for payload in (b"", b"{", b"null", b"[]", valid + b" {}", valid + b" garbage"):
            with self.subTest(payload=payload):
                self.assert_refused(payload)

    def test_duplicate_fields_including_escaped_names(self):
        for suffix in (
            b',"source":"other"}',
            b',"sour\\u0063e":"other"}',
            b',"contract":"pipelang.lexer.request.v1"}',
        ):
            with self.subTest(suffix=suffix):
                self.assert_refused(self.encode(self.request(""))[:-1] + suffix)

    def test_unknown_and_missing_fields(self):
        requests = (
            {},
            {"contract": REQUEST_CONTRACT},
            {"source": ""},
            {"contract": REQUEST_CONTRACT, "source": "", "extra": None},
        )
        for request in requests:
            with self.subTest(request=request):
                self.assert_refused(self.encode(request))

    def test_wrong_types_and_contracts(self):
        requests = [self.request(value) for value in (None, False, 123, [], {})]
        requests += [
            {"contract": value, "source": ""}
            for value in (None, False, 123, [], {}, "", "pipelang.lexer.request.future")
        ]
        for request in requests:
            with self.subTest(request=request):
                self.assert_refused(self.encode(request))

    def test_invalid_utf8_json_source(self):
        self.assert_refused(b'{"contract":"pipelang.lexer.request.v1","source":"\xff"}')

    def test_unpaired_json_surrogate(self):
        self.assert_refused(b'{"contract":"pipelang.lexer.request.v1","source":"\\ud800"}')

    def test_input_byte_limit(self):
        self.assert_refused(b" " * (MAXIMUM_INPUT_BYTES + 1))

    def test_source_byte_limit(self):
        self.assert_refused(self.encode(self.request(" " * (MAXIMUM_SOURCE_BYTES + 1))))
        # This multibyte string is below the character limit but above the byte limit.
        self.assert_refused(self.encode(self.request("é" * (MAXIMUM_SOURCE_BYTES // 2 + 1))))

    def test_maximum_source_bytes_are_accepted(self):
        self.assert_response(" " * MAXIMUM_SOURCE_BYTES, {
            "contract": RESPONSE_CONTRACT,
            "tokens": [{
                "kind": "eof",
                "literal": "",
                "start": MAXIMUM_SOURCE_BYTES,
                "end": MAXIMUM_SOURCE_BYTES,
            }],
            "failure": None,
        })

    def test_input_depth_limit(self):
        process = self.assert_refused(b"[" * 18 + b"0" + b"]" * 18)
        self.assertIn(b"nesting limit", process.stderr)

    def test_input_node_limit(self):
        process = self.assert_refused(b"[" + b"null," * 64 + b"null]")
        self.assertIn(b"node limit", process.stderr)


if __name__ == "__main__":
    unittest.main()
