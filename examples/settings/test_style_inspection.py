#!/usr/bin/env python3
"""Independent authored settings-style metadata and binding-receipt oracle."""
import hashlib
import json
from pathlib import Path
import sys


def main():
    directory = Path(sys.argv[1])
    descriptor = json.loads((directory / "style.json").read_text())
    expected = {
        "protocol": "pipelang.native-style-descriptor.v1",
        "view": "SettingsView",
        "style": "SettingsTheme",
        "scope": "resolved-authored",
        "properties": [
            {"name": "background", "type": "color", "value": "#142434FF"},
            {"name": "controlHeight", "type": "length", "unit": "px", "value": 42},
            {"name": "focus", "type": "color", "value": "#66CCFFFF"},
            {"name": "gap", "type": "length", "unit": "px", "value": 10},
            {"name": "padding", "type": "length", "unit": "px", "value": 16},
            {"name": "text", "type": "color", "value": "#EAF1F8FF"},
        ],
    }
    assert descriptor == expected, descriptor
    receipt = json.loads((directory / "bindings.json").read_text())
    assert set(receipt["generated"]) == {"Model.hpp", "model.json", "ModelView.hpp", "view.json", "style.json"}
    for name, expected_hash in receipt["generated"].items():
        assert hashlib.sha256((directory / name).read_bytes()).hexdigest() == expected_hash, name
    print("settings_authored_style_metadata_and_complete_receipt=passed")


if __name__ == "__main__":
    main()
