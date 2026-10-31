# Editor tooling status — 2026-10-31

The extension source and Node tests remain. Run `node extension.test.js` in this
directory; no npm install is required. The old Python packaging/publication
workflows and unused VSCE dependency tree were retired. Marketplace delivery
requires a separately qualified native packaging path. Configure diagnostics to
use the installed C++ `pipelang` executable.

# PipeLang Language Support

This extension owns `.pipe` language configuration, syntax highlighting, snippets,
keyword completion and diagnostics from the standalone `pipelang check` command.
Set `pipelang.executablePath` to select an installed compiler explicitly. Compiler
requests use unsaved document text, cancel stale requests and respect configurable
time/output budgets. Missing compilers do not produce invented diagnostics.

Dockpipe's workflow/package authoring extension is separately owned by Dockpipe.

`CHANGELOG.md` preserves historical language compatibility notes and their original
migration text. The current testing instructions are in this README; historical packaging paths
do not define dependencies.
