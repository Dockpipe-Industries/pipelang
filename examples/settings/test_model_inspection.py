#!/usr/bin/env python3
"""Independent semantic checks for the native typed-model inspection frames."""
import json
from pathlib import Path
import sys


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def controls(frame):
    require(frame["schema"] == "pipelang.pipeview.inspect.v3", "Inspection contract differs")
    items = frame["controls"]
    require(len({item["id"] for item in items}) == len(items), "Duplicate control identity")
    return {item["id"]: item for item in items}


def main():
    path = Path(sys.argv[1])
    require(path.stat().st_size <= 1 << 20, "Inspection fixture exceeds capacity")
    report = json.loads(path.read_text(encoding="utf-8"))
    require(set(report) == {"initial", "default", "edited", "saving", "saved", "reverted"},
            "Inspection frame identities differ")
    expected = {
        "initial": ("Rider", True, "100", False, False, "Saved"),
        "edited": ("RiderX", False, "99", False, True, "Unsaved changes"),
        "saving": ("RiderX", False, "99", True, False, "Saving"),
        "saved": ("RiderX", False, "99", False, False, "Saved"),
        "reverted": ("RiderX", False, "99", False, False, "Saved"),
    }
    for name, (player, vsync, volume, saving, can_save, status) in expected.items():
        frame = controls(report[name])
        for identity, value in [("name-field", player), ("vsync-toggle", vsync),
                                ("volume-stepper", volume), ("save-status", status)]:
            require(frame[identity].get("value") == value, name + ": typed value differs")
            require(not frame[identity]["redacted"], name + ": explicit exposure was ignored")
        require(frame["save-button"]["enabled"] == can_save,
                name + ": Save availability differs from authored property")
        for identity in ["name-field", "vsync-toggle", "volume-stepper", "revert-button"]:
            require(frame[identity]["enabled"] == (not saving),
                    name + ": pending availability differs")
        require(frame["save-error"].get("value") == "", name + ": unexpected persistence detail")
    redacted = controls(report["default"])
    for identity in ["name-field", "vsync-toggle", "volume-stepper"]:
        require(redacted[identity]["redacted"] and "value" not in redacted[identity],
                "Default inspection disclosed protected model data")
    require(redacted["save-status"].get("value") == "Saved", "Public status is unavailable")
    print("native_model_inspection_types_availability_and_redaction=passed")


if __name__ == "__main__":
    main()
