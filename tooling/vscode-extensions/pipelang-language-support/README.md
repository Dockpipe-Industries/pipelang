# PipeLang Language Support

This extension owns `.pipe` language configuration, syntax highlighting, snippets,
keyword completion and diagnostics from the standalone `pipelang check` command.
Set `pipelang.executablePath` to select an installed compiler explicitly. Compiler
requests use unsaved document text, cancel stale requests and respect configurable
time/output budgets. Missing compilers do not produce invented diagnostics.

Build and test with an absolute pinned `DOCKPIPE_BIN` and
`workflows/editor.build/config.yml`. Node 22 or newer is required. The workflow emits
an inspected VSIX and its exact SHA-256; publication is a separate manual workflow.
Dockpipe's workflow/package authoring extension is separately owned by Dockpipe.

`CHANGELOG.md` preserves historical language compatibility notes and their original
migration text. The current build and publication instructions are in this README
and the editor workflow documentation; historical paths do not define dependencies.
