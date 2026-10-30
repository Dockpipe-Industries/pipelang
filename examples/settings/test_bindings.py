import copy
import unittest
import generate_bindings as bindings


class Admission(unittest.TestCase):
    def setUp(self):
        string = {"kind": "primitive", "primitive": "string"}
        boolean = {"kind": "primitive", "primitive": "bool"}
        self.action = {"contract": "pipelang.native-cpp-export.v1", "profile": "pipelang.cpp.pilot.v1",
                       "language_contract": "v0.113.0", "compiler_contract": "pipelang.compiler.v1",
                       "source_sha256": "1" * 64, "generated_sha256": "2" * 64,
                       "compiler_executable_sha256": "3" * 64,
                       "functions": [{"name": "IsValidName", "cpp_symbol": "pipelang::fn_" + "a" * 24,
                                      "identity": {"package_id": "pipeview.settings.showcase",
                                                   "path": "app.root.settings.isvalidname",
                                                   "callable": {"parameters": [string], "returns": boolean}},
                                      "parameters": [{"position": 0, "name": "value", "type": string}],
                                      "return_type": boolean}]}
        integer = {"kind": "numeric", "numeric": {"representation": "integer", "bits": 64, "signed": True}}
        semantic_int = {"kind": "primitive", "primitive": "int"}
        self.action["functions"].append({
            "name": "IsValidVolume", "cpp_symbol": "pipelang::fn_" + "b" * 24,
            "identity": {"package_id": "pipeview.settings.showcase", "path": "app.root.settings.isvalidvolume",
                         "callable": {"parameters": [semantic_int], "returns": boolean}},
            "parameters": [{"position": 0, "name": "value", "type": integer}], "return_type": boolean})
        self.view = {"contract": "pipelang.pipeview.export.v1", "source_contract": "pipelang.pipeview.source.v2",
                     "action_contract": "pipelang.pipeview.state.v1", "adapter": "forgepipe.ui.v2",
                     "source_sha256": "1" * 64, "generated_sha256": "2" * 64,
                     "compiler_executable_sha256": "3" * 64, "state_runtime_sha256": "4" * 64}

    def test_exact_admission(self):
        self.assertEqual(bindings.admit_actions(self.action, "1" * 64, "2" * 64, "3" * 64, "", ""),
                         {"IsValidName": "pipelang::fn_" + "a" * 24, "IsValidVolume": "pipelang::fn_" + "b" * 24})
        bindings.admit_view(self.view, "1" * 64, "2" * 64, "3" * 64, "")

    def test_explicit_xml_syntax(self):
        with self.assertRaises(ValueError):
            bindings.admit_view(self.view, "1" * 64, "2" * 64, "3" * 64, "", "xml")
        self.view["source_syntax"] = "xml"
        self.view["output_format"] = "header"
        bindings.admit_view(self.view, "1" * 64, "2" * 64, "3" * 64, "", "xml")
        with self.assertRaises(ValueError):
            bindings.admit_view(self.view, "1" * 64, "2" * 64, "3" * 64, "")

    def test_xml_output_format_admission(self):
        self.view["source_syntax"] = "xml"
        for output_format in ("xml", "pipelang", "", None):
            self.view["output_format"] = output_format
            with self.subTest(output_format=output_format), self.assertRaises(ValueError):
                bindings.admit_view(self.view, "1" * 64, "2" * 64, "3" * 64, "", "xml")

    def test_selected_native_view_parser_identity(self):
        selected = "5" * 64
        with self.assertRaises(ValueError):
            bindings.admit_view(self.view, "1" * 64, "2" * 64, "3" * 64, "", view_parser_hash=selected)
        self.view["view_parser_executable_sha256"] = selected
        with self.assertRaises(ValueError):
            bindings.admit_view(self.view, "1" * 64, "2" * 64, "3" * 64, "")
        bindings.admit_view(self.view, "1" * 64, "2" * 64, "3" * 64, "", view_parser_hash=selected)
        self.view["view_parser_executable_sha256"] = "6" * 64
        with self.assertRaises(ValueError):
            bindings.admit_view(self.view, "1" * 64, "2" * 64, "3" * 64, "", view_parser_hash=selected)

    def test_native_view_parser_does_not_admit_xml(self):
        self.view["source_syntax"] = "xml"
        self.view["output_format"] = "header"
        self.view["view_parser_executable_sha256"] = "5" * 64
        with self.assertRaises(ValueError):
            bindings.admit_view(self.view, "1" * 64, "2" * 64, "3" * 64, "", "xml", "5" * 64)

    def test_each_identity_field(self):
        for key in self.view:
            altered = copy.deepcopy(self.view)
            altered[key] = "tampered"
            with self.subTest(key=key), self.assertRaises(ValueError):
                bindings.admit_view(altered, "1" * 64, "2" * 64, "3" * 64, "")
        for key in ("contract", "profile", "language_contract", "compiler_contract", "source_sha256",
                    "generated_sha256", "compiler_executable_sha256"):
            altered = copy.deepcopy(self.action)
            altered[key] = "tampered"
            with self.subTest(key=key), self.assertRaises(ValueError):
                bindings.admit_actions(altered, "1" * 64, "2" * 64, "3" * 64, "", "")

    def test_validator_signature(self):
        for key in ("identity", "name", "parameters", "return_type", "cpp_symbol"):
            altered = copy.deepcopy(self.action)
            altered["functions"][0][key] = "tampered"
            with self.subTest(key=key), self.assertRaises(ValueError):
                bindings.admit_actions(altered, "1" * 64, "2" * 64, "3" * 64, "", "")

    def test_volume_identity_and_legacy_capability_refusal(self):
        for key in ("identity", "name", "parameters", "return_type", "cpp_symbol"):
            altered = copy.deepcopy(self.action)
            altered["functions"][1][key] = "tampered"
            with self.subTest(key=key), self.assertRaises(ValueError):
                bindings.admit_actions(altered, "1" * 64, "2" * 64, "3" * 64, "", "")
        altered = copy.deepcopy(self.action)
        altered["functions"][1]["cpp_symbol"] = altered["functions"][0]["cpp_symbol"]
        with self.assertRaises(ValueError):
            bindings.admit_actions(altered, "1" * 64, "2" * 64, "3" * 64, "", "")
        self.view["adapter"] = "forgepipe.ui.v1"
        with self.assertRaises(ValueError):
            bindings.admit_view(self.view, "1" * 64, "2" * 64, "3" * 64, "")

    def test_duplicate_functions_and_components(self):
        self.action["functions"].append(copy.deepcopy(self.action["functions"][0]))
        with self.assertRaises(ValueError):
            bindings.admit_actions(self.action, "1" * 64, "2" * 64, "3" * 64, "", "")
        with self.assertRaises(ValueError):
            bindings.admit_view(self.view, "1" * 64, "2" * 64, "3" * 64, "f" * 64)
        with self.assertRaises(ValueError):
            bindings.closed_object([("contract", 1), ("contract", 2)])


if __name__ == "__main__":
    unittest.main()
