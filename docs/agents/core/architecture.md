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
