# Native typed PipeView XML

PipeView has two explicitly selected authoring formats: `.pview` for declarative
views and `.pxml` for XML. Ordinary PipeLang models remain `.pipe`. Generic XML
files remain `.xml`; `.vpipe` is not a PipeView extension. The file-owning SDK
boundary selects the syntax. The byte-oriented `pipelang.native-model-view.v1`
request accepts `source_format: "pview"` or `source_format: "pxml"`; omission
preserves the existing declarative protocol behavior. It never guesses syntax
from the content. Neither format adds views to ordinary `.pipe` model discovery.

The native XML frontend projects checked syntax tokens, with authored XML byte
ranges, into the existing typed view and style checker. Both formats use the
same `View`/`Node` model, manifest validation, adapter emitter, style resolver and
optional style descriptor. There is no XML runtime, alternate schema, renderer,
Qt dependency or interpreter. Typed action return values, async execution and
cancellation metadata retain the shared checker and generated adapter contracts.

## Document contract

```xml
<View name="SettingsView" model="SettingsModel"
      profile="pipelang.native-model-view.v1" style="Theme">
    <Style name="Theme">
        <Token name="inset" type="length" value="16px"/>
        <Property name="padding" value="inset"/>
        <Property name="text" value="#EAF1F8FF"/>
    </Style>
    <Column id="root">
        <TextField id="name" value="{bind model.draft.playerName}"
                   changed="{model.Rename}" sensitive="true"/>
        <Button id="save" label="Save" enabled="{bind model.CanSave()}"
                clicked="{model.Save}"/>
    </Column>
</View>
```

`View` requires `name`, `model` and the exact native `profile`; `style` is optional.
Zero or more `Style` declarations precede exactly one control root. The external
model is supplied by the ordinary-model manifest. Model fields and pure scalar
getters use `{bind model.member}` and `{bind model.Getter()}`; events reference
typed methods as `{model.Method}`. Application state and logic stay in that model.

The supported controls are `Column`, `Row`, `Label`, `Button`, `Toggle`,
`TextField` and `PercentStepper`. Attributes correspond to the same declarative
properties: `id`, `text`/`label`, `value`, `enabled`, `sensitive`, `width`, `height`,
`changed` and `clicked`. The shared checker enforces which properties and event
types apply to each control. Booleans use `true`/`false`; dimensions use decimal
integers. Literal text and IDs are XML attribute strings, never source fragments.
Only layout controls contain children; leaf controls may be self-closing or have
an empty paired closing tag. Child order is preserved. Unknown or inactive
attributes, extra roots, text children and unknown controls fail explicitly.

`Style` requires `name` and optionally `extends`. Its children are
`Token name="..." type="length|color" value="..."` and
`Property name="..." value="..."`; both optionally accept `override="true|false"`.
Values are logical pixel lengths, `#RRGGBBAA` colors or token names. XML style
members use the existing declaration, inheritance, type, range, cycle and override
rules, including validation of unselected styles. A `Theme.pxml` test fragment
contains style elements for insertion inside `View`; it is not a standalone view.

## XML admission and diagnostics

The document is UTF-8 XML 1.0. An initial UTF-8 BOM and XML declaration are
optional. The declaration supports version `1.0`, optional UTF-8 encoding and
optional `standalone="yes|no"`. Both quote styles and comments are supported.
Attribute whitespace follows XML normalization: literal CR/LF/tab become spaces,
while numeric character references preserve their represented characters.
The five predefined entities and decimal/hexadecimal character references are
supported. Malformed UTF-8, invalid XML scalars, duplicate attributes, unclosed or
mismatched tags and invalid references fail without repair or replacement.

This is a bounded typed dialect, not a general XML document processor. Names are
unqualified ASCII names. Namespaces, DTDs, entity declarations, external resources,
CDATA, processing instructions and content text are unsupported. No file, network
or entity expansion operation occurs. The source limit is 65,536 bytes. The shared
view limits remain 128 controls and depth 8. XML admits at most 16 attributes per
element, 32 styles, 128 members per style and 16,384 projected tokens; the shared
style checker additionally retains its token, inheritance and resolution limits.

XML syntax diagnostics use `PMV3001`; typed view diagnostics retain `PMV1001` and
style diagnostics retain `PMV2001`. Ranges refer to original UTF-8 XML byte
offsets, generally the offending attribute or opening tag. JSON/format/manifest
admission errors retain `PMV0001`. There is no canonical-source offset masquerading
as an authored XML location.

## Historical profiles and qualification

The retained `SettingsView.xml` and frozen legacy-settings fixtures describe the
historical `pipelang.pipeview.source.v2` runtime with inline `State` and
`Action`/set/save/revert declarations. Their filenames and bytes remain historical
evidence. The native XML frontend explicitly refuses that profile and those
declarations; renaming an old file does not convert its runtime semantics.

The active `examples/settings/SettingsView.pxml` instead matches the active
`SettingsView.pview`, including its existing `SettingsModel` methods and styles.
The native process oracle compares exact generated headers, view manifests and
style descriptors for both syntaxes, escaping, dimensions, enabled state and
the active settings example. XML refusal cases exercise shared type failures,
authored spans, syntax safety and capacity limits. Exact generated adapter parity
covers the transport implementation; installed runtime, sanitizer, platform and
interactive qualification remain separate checks. A staged implementation alone
does not establish any of those acceptance claims.


Local Linux/amd64 qualification completed on 2026-11-03. The declared native
build, check, default test and offline package/relocation workflows passed. The
389-case native model/view oracle passed normally and with the XML frontend
instrumented with ASan/UBSan. All six Settings runtime/inspection tests passed
for both source formats normally and under sanitizers; both affected Forge
native examples also passed in both modes. The installed engine implementation
was not newly sanitizer-qualified. Forge example configuration retains its
existing Python discovery dependency; selected native C++ tests execute.

The qualified SDK manifest is
`aeaa856d403db28f3a1f56e9fdd97464212daf6696d865ed37a934fc600bc57a`.
Local receipts, archive identity and scope limits are recorded in
`/tmp/engine-domain-boundaries-20261101/PIPEVIEW-NATIVE-FORMATS-QUALIFICATION-20261103.md`.
Resource limits and serial builds were preserved. Interactive UI, Windows/macOS,
hosted delivery and publication remain unqualified; release pins and live SDK
selections were not changed.
