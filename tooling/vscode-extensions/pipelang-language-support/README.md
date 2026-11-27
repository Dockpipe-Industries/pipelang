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

## Debug native executables

Build a PipeLang executable with the native backend and the `debug` profile.
This extension enables breakpoints in `.pipe` files. Install the LLVM LLDB DAP
extension and make `lldb-dap` available on PATH, or set
`lldb-dap.executable-path` to its absolute path.

For a multi-file project with sources under `src`, add this to `.vscode/launch.json`
and set `program` to the executable your build produced.

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "name": "Debug PipeLang",
      "type": "lldb-dap",
      "request": "launch",
      "program": "${workspaceFolder}/build/bin/app",
      "cwd": "${workspaceFolder}",
      "sourceMap": [["project", "${workspaceFolder}/src"]]
    }
  ]
}
```

Set a breakpoint on an authored statement and start **Debug PipeLang**. Use the
actual `SOURCE_ROOT` directory in the source map. For a single-file build, map
`.` to the directory containing the source. Keep the PDB beside the Windows
executable and the dSYM beside the macOS executable when moving build outputs.
