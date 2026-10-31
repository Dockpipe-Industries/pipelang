# Experimental PipeLang view source

Status: bounded source profile implemented on 2026-10-28. Select it explicitly
through `pipelang-viewc` or `pipelang.ParseViewSource`; it is not admitted by the
ordinary class/module compiler entrypoints. This additive lane does not change
an existing language contract or claim general reactive-language support.

The event-only profile below remains supported. New typed state/actions and Save
use the explicit [v2 profile](pipeview-state-actions.md) and the PipeLang-owned
[settings showcase](../../examples/settings/README.md).

## Authoring a small settings view

```pipe
view SettingsView {
    state playerName: string = "Rider";
    state vsync: bool = true;
    event nameEdited(string);
    event vsyncEdited(bool);
    event revert();

    Column(id: "root") {
        Label(id: "title", text: "Settings");
        Label(id: "name-label", text: "Player name");
        TextField(id: "name-field", value: bind playerName, changed: nameEdited);
        Toggle(id: "vsync-toggle", label: "VSync", value: bind vsync, changed: vsyncEdited);
        Button(id: "revert-button", label: "Revert", clicked: revert);
    }
}
```

`state` declares an initial typed binding in this view profile. `event` declares
an interaction payload expected by the host bridge. Neither declaration adds
mutable classes, effectful actions or a reactive state runtime to ordinary
PipeLang. `bind name` resolves to a declared state; bare event references resolve
to declared events. They are checked references, not strings looked up at runtime
by the compiler. Host dispatch still needs an explicitly admitted bridge.

Supported initial state types are `bool`, `string` and signed 64-bit `int`.
Initializers are literals; text uses the existing PipeLang lexer and escape
rules. Booleans, decimal integers (including negatives) and text are supported.
No arithmetic, calls, computed values or implicit conversions occur in a view.
An event has either no payload or exactly one supported scalar payload type.

There is exactly one named view and one root component per source file. State
and event declarations precede the root and share one declaration namespace.
Declarations must be used; unused state/events refuse rather than disappearing
from the generated document. Components take named properties and either a
semicolon or a child block. A leaf may have an empty block, but cannot have child
components. Containers may have no children. Trailing property commas, positional
arguments, multiple roots, duplicate properties and trailing source refuse.

| Component | Properties beyond required text-literal `id` | Interaction type |
| --- | --- | --- |
| `Column`, `Row` | None; own child components | None |
| `Label` | Optional `text`; optional `value: bind state` | Read-only scalar display |
| `TextField` | Required string `value: bind state`, `changed: event` | `event(string)` |
| `Toggle` | Optional literal `label`; required bool `value: bind state`, `changed: event` | `event(bool)` |
| `PercentStepper` | Optional literal `label`; required int `value: bind state`, `changed: event` | `event(int)` |
| `Button` | Optional literal `label`; required `clicked: event` | `event()` |

This is the first component profile, not a promise that those component names
become permanent language keywords. The compiler parser keeps component/property
references generic; the optional SDK owns the concrete component catalog and
property/type checks. Custom components, model parameters, packages/imports,
styles, XML, lists and conditional layout need their own accepted extensions.

## Ownership and artifacts

- `src/lib/pipelang/view_syntax.go` owns syntax trees, spans and bounded parsing
  over the existing Go/reference or explicitly selected native lexer.
- `sdk/pipeview/source` owns component/property/binding/event admission and lowers
  into the existing backend-neutral `sdk/pipeview` declarations.
- `sdk/pipeview/forgepipe` remains the explicitly selected renderer-transport
  projection. The source-checking package imports neither it nor engine code.
- `src/cmd/pipelang-viewc` orchestrates source admission and the initial ForgePipe
  output profile. It produces an inert header and metadata; it starts no UI.

Source contract: `pipelang.pipeview.source.v1`. Export contract:
`pipelang.pipeview.export.v1`. The CLI selects `forgepipe.ui.v1` for existing controls and `forgepipe.ui.v2`
when PercentStepper is present; it retains the same output
with the existing legacy scale/focus profile; it does not implement PipeView
styles. No Core/Application IR schema, existing language version or frozen
compatibility fixture changes. Go remains the default/reference implementation.

Build with `PIPELANG_BUILD_PIPEVIEW_EXAMPLE=1` using the existing build workflow
and installed DockPipe. Then explicitly select the source and artifact:

```sh
/absolute/path/to/pipelang-viewc \
  --source /absolute/path/to/SettingsView.pipe \
  --output /absolute/path/to/build/PipeViewDocument.hpp
```

The output parent must already exist. `<output>.json` is a completion marker
binding the source, generated bytes, compiler executable, source/export contracts
and adapter identity. Consumers must verify it against the expected tool/source
hashes and generated bytes. Output paths must be regular files or absent; errors
before publication preserve existing outputs. A partially replaced header with
stale metadata cannot pass consumer digest admission. Generated byte escapes
preserve UTF-8/control bytes without relying on C++ source-file encoding.

`--cpp-lexer /absolute/path --cpp-lexer-sha256 EXACT_LOWERCASE_SHA256` explicitly
selects the experimental native lexer. Supply both flags; failure refuses without
fallback. View parsing and component semantics remain Go code on either lexer
path. The action compiler's separately selected C++ emitter is unchanged.

Source/syntax limits are 65536 bytes, 128 state declarations, 128 event
declarations, 128 component nodes and eight component levels. Existing neutral
and adapter limits still apply: 4096-byte text, 128-byte neutral identities,
64-byte ForgePipe identities and a 65536-byte ForgePipe transport. Diagnostics
reuse PipeLang codes/categories and byte spans. Most binding/property errors
point to the offending reference; final shared-document constraint failures can
point to the root component. More precise related-location diagnostics remain
an editor improvement.

## Settings proof and remaining work

The earlier ForgePipe source-only `examples/pipelang-settings` integration fixture uses this view and
a separately compiled `Settings.pipe` class for `DefaultName` and `DefaultVSync`.
The host's Revert event calls those admitted functions. The VSync binding is
prototype UI state; it does not change native presentation mode. That v1 fixture does not persist settings; the new PipeLang-owned v2 showcase does. The new settings example is not yet included in an installed/released
SDK payload; the existing SDK is consumed through its installed CMake targets.

ForgePipe Session still pre-applies text/toggle edits before emitting events.
This slice does not claim the planned checked-action state mutation model.
Save/dirty/validation/cancellation, enums/checked Percent controls, ownership and
async semantics, Qt/QML proof, live AI inspection and hot reload remain on the
[roadmap](../agents/backlog.md#pipeview-modular-typed-ui-roadmap). The declaration
inspection contract and backend separation remain intact.

See [verification evidence](../research/pipeview-source-syntax.md).

PercentStepper uses the existing syntax and int payload contract. It has no
min/max/step properties: native gestures use 0..100 in steps of one, while raw
model drafts remain int64. Action views require the set target to match the
bound state. New-component declaration/inspection/native capability identities
are versioned separately from source grammar; see the
[percent contract](../research/pipeview-percent-stepper.md).
