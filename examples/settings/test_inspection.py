#!/usr/bin/env python3
# Legacy scalar/source-v2 compatibility fixture. Current applications use the installed native model SDK.
"""Compare native inspection frames with the independent Go reference report."""

import json
from pathlib import Path
import sys


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def read_report(path):
    source = Path(path)
    require(source.stat().st_size <= 1 << 20, "Inspection report exceeds fixture capacity")
    return json.loads(source.read_text(encoding="utf-8"))


def nodes(root):
    yield root
    for child in root.get("children", []):
        yield from nodes(child)


def verify_report(report):
    require(set(report) == {"initial", "default", "edited", "saving", "saved", "reverted"},
            "Inspection frame identities differ")
    expected = {
        "initial": ("Rider", True, False, False, False, "Saved"),
        "edited": ("RiderX", False, True, False, True, "Unsaved changes"),
        "saving": ("RiderX", False, True, True, False, "Saving"),
        "saved": ("RiderX", False, False, False, False, "Saved"),
        "reverted": ("RiderX", False, False, False, False, "Saved"),
    }
    for name, (player, vsync, dirty, saving, can_save, status) in expected.items():
        frame = report[name]
        require(set(frame) == {"schema", "root", "bindings", "actions", "model"},
                name + " contains unexpected inspection fields")
        require(frame["schema"] == "pipelang.pipeview.inspect.v2", "Inspection schema differs")
        require(frame["bindings"]["playerName"] == {
            "type": "string", "readonly": False, "redacted": False, "value": player,
        }, name + " player binding differs")
        require(frame["bindings"]["vsync"] == {
            "type": "bool", "readonly": False, "redacted": False, "value": vsync,
        }, name + " Boolean binding differs")
        require(type(frame["bindings"]["vsync"]["value"]) is bool,
                name + " Boolean binding is not a JSON Boolean")
        require(frame["bindings"]["volume"] == {"type": "int64", "readonly": False, "redacted": False,
                "value": "100" if name == "initial" else "99"}, "Volume inspection changed")
        require(frame["bindings"]["save.status"] == {
            "type": "string", "readonly": True, "redacted": False, "value": status,
        }, name + " derived status binding differs")
        require(frame["model"] == {
            "dirty": dirty, "valid": True, "saving": saving, "can_save": can_save, "status": status,
        }, name + " model flags differ")
        for flag in ("dirty", "valid", "saving", "can_save"):
            require(type(frame["model"][flag]) is bool, name + " model flag is not a JSON Boolean")
        controls = {node["id"]: node for node in nodes(frame["root"])}
        require(controls["save-button"]["action_available"] == can_save,
                name + " Save availability differs")
        for identity in ("name-field", "vsync-toggle", "volume-stepper", "revert-button"):
            require(controls[identity]["action_available"] == (not saving),
                    name + " edit/revert availability differs")
        for node in controls.values():
            require("text" in node, "Static control text is missing")
            require(("action_available" in node) == ("action" in node),
                    "Action availability is attached to a passive control")

    redacted = report["default"]
    require(redacted["root"] == report["initial"]["root"], "Redaction changed the semantic tree")
    require(redacted["actions"] == report["initial"]["actions"], "Redaction changed typed actions")
    require(redacted["model"] == report["initial"]["model"], "Redaction changed model flags")
    for identity, binding in redacted["bindings"].items():
        exposed = report["initial"]["bindings"][identity]
        require(binding == {"type": exposed["type"], "readonly": exposed["readonly"], "redacted": True},
                "Default inspection leaked or omitted binding metadata")


def main():
    require(len(sys.argv) == 3, "Usage: test_inspection.py NATIVE_REPORT GO_REPORT")
    native = read_report(sys.argv[1])
    reference = read_report(sys.argv[2])
    verify_report(native)
    verify_report(reference)
    require(json.dumps(native, sort_keys=True) == json.dumps(reference, sort_keys=True),
            "Native inspection differs from the independent Go reference")
    print("PipeView inspection: native/reference frames, redaction and action availability passed")


if __name__ == "__main__":
    main()
