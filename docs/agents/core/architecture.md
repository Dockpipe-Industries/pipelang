# Architecture

PipeLang owns its compiler, semantic model, application IR, backends, standalone
CLI and public materializer SDK. Dockpipe owns orchestration and execution.
Dockpipe integrations import a published version of this module; no local
checkout replace directives or duplicated compiler implementation belong there.

The configuration compiler and materializer live in `src/native/legacy-compiler`;
`pipelang` is its installed CLI. The active scalar frontend lives in
`src/native/scalar-compiler` and delegates to the checksum-selected native emitter.
Ordinary class/record/effect semantics live in `src/native/model-compiler`;
`src/native/model-view-compiler` resolves exported model members into an optional
ForgePipe bridge. The owned-value/async runtime and exact installed SDK live in
`sdk/pipeview/model`. SDK tooling, manifest admission and native archiving live in
`src/native/sdk-tools`; bounded process/files/hash support and containment have
separate native owners. Historical Go APIs/Core/stream profiles and Python drivers
have been retired. Useful frozen language/JSON regression data remains.
No application-specific Set/Save/Revert opcode list belongs in the model bridge.

`dockpipe.config.json` workflow roots and `dockpipe.operation_event.v1` are
interoperability data contracts. Their use does not authorize importing engine
implementation. Preserve source and artifact compatibility across extraction.


## Modular packages and PipeView

Keep optional library and backend responsibilities behind explicit package APIs
inside this repository. A single checkout must not force headless consumers to
link UI runtimes or force one UI backend to depend on another. General language
semantics remain compiler/foundation-owned; UI components, styling, frontend
conversion, tooling and adapters keep their own cohesive owners. Introduce these
boundaries incrementally rather than reorganizing unrelated existing code.

The [PipeView roadmap](../backlog.md#pipeview-modular-typed-ui-roadmap) records the
planned package dependency boundaries and acceptance slices. It distinguishes locally accepted bounded slices from planned runtime and
language features. The [experimental view source profile](../../concepts/pipeview-source.md)
adds an explicit compiler parser entrypoint and optional SDK checker; ordinary
class/module grammar and Core/Application IR contracts remain unchanged.
The [existing Qt boundary](../../concepts/pipelang-qt-boundary.md) remains the
current contract until explicitly extended and qualified.

The native model-view compiler owns typed view-scoped style resolution in
`styles.cpp`, independently of renderers and model/action semantics. The
[style capability](../../concepts/pipeview-styles.md) maps resolved values to the
existing optional adapter contract; broader selectors/parts/reload remain planned.

The optional native authored-style descriptor projects the same checked style
used by header emission. SDK tooling validates its versioned transport and
publishes it atomically with bindings; it does not parse or reinterpret source.
The descriptor remains renderer-free and distinct from effective runtime state.
