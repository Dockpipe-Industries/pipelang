# Architecture

PipeLang owns its compiler, semantic model, application IR, backends, standalone
CLI and public materializer SDK. Dockpipe owns orchestration and execution.
Dockpipe integrations import a published version of this module; no local
checkout replace directives or duplicated compiler implementation belong there.

The compiler API remains in `src/lib/pipelang`. Application IR lives in
`src/lib/applicationir`, standalone CLI dispatch in `src/lib/application.Run`,
and shared materialization in `sdk/materialize`. Backend-specific entrypoints
remain separately selected and must retain their explicit capability boundaries.

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
