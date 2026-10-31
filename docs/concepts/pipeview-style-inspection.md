# Resolved authored style inspection

The explicitly selected native model-view compiler supports the renderer-free
`pipelang.native-style-descriptor.v1` contract. Tooling can inspect typed style
results without parsing generated C++ or exposing application model values.
This is compile-time metadata about authored style properties. It does not report
backend defaults, current runtime overrides, computed geometry or host scale.

An ordinary `pipelang.native-model-view.v1` request may select:

```json
{"style_inspection": "pipelang.native-style-descriptor.v1"}
```

This is an additional member of the existing complete request, not a standalone
request. Success then adds `style_descriptor`; its ordinary generated header and
view manifest remain identical. Omitting selection preserves the complete default
response bytes. Boolean switches, unknown versions and malformed selections
refuse. Source/type failures return diagnostics without a success descriptor.

The descriptor has exactly `protocol`, `view`, `style`, `scope` and `properties`:

```json
{
  "protocol": "pipelang.native-style-descriptor.v1",
  "view": "SettingsView",
  "style": "SettingsTheme",
  "scope": "resolved-authored",
  "properties": [
    {"name": "gap", "type": "length", "unit": "px", "value": 10},
    {"name": "text", "type": "color", "value": "#EAF1F8FF"}
  ]
}
```

Properties contain only resolved, selected authored values in name order.
Inheritance and token overrides use the same checked style object as header
emission. Lengths are finite non-Boolean numbers in logical `px`, constrained by
the existing property profile. Colors are canonical uppercase `#RRGGBBAA`.
Unknown, duplicate, unsorted or incorrectly typed properties refuse admission.
The style name is null when the view selects no style; properties are then empty.
An explicitly selected empty style retains its name and an empty property array.
Unselected style declarations, token names and model/runtime values are omitted.
The [style contract](pipeview-styles.md) owns source grammar, types and limits.

The exact installed SDK exposes explicit build-time selection:

```cmake
pipelang_add_model_bindings(settings-bindings
    STYLE_INSPECTION
    MODEL "${CMAKE_CURRENT_SOURCE_DIR}/Settings.pipe"
    VIEW "${CMAKE_CURRENT_SOURCE_DIR}/SettingsView.pipe"
    NAMESPACE settings_models
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/compiled-ui")
```

The generator CLI uses `--style-inspection`. Selection atomically publishes
`style.json` alongside the five existing binding outputs; `bindings.json` hashes
the sidecar with the other generated artifacts. Default generation keeps its
five-file output and prior response/receipt shapes. Turning selection off removes
the previous generator-owned sidecar as part of the complete output replacement.
Unknown user files and symlinks refuse; invalid/mismatched descriptors or failed
publication preserve previous complete outputs. Sources, tools and installed
SDK content remain checked before and after compilation. There is no fallback to
an older tool or weaker descriptor format.

Settings explicitly selects the feature. Independent native/default-wire checks,
SDK hostile-input/rollback tests and actual installed-generator transitions
complement its descriptor/receipt and behavior CTests. These are compile-time and
headless checks. Runtime semantic-tree integration, component/state styling,
backend extension, Qt mappings, hot reload and visible UI qualification remain
separate work. No development endpoint or additional renderer dependency is added.

The installed integration fixture exercises both direct generator selection and
CMake reconfiguration in one build directory, checking that changing selection
updates the owned sidecar while preserving ordinary generated artifact bytes.
