# PipeLang

PipeLang is a C++ compiler and SDK repository. DockPipe runs development builds,
checks, tests and packaging. No Go or Python source, manifests, interpreters or
compiler toolchains are required. Vendored C++ dependencies build offline.

The supported native entrypoints are:

- `pipelang`: configuration checking, compilation, invocation and materialization;
  configuration profile `v0.0.0.1`, also used by editor diagnostics.
- `pipelang-modelc`: typed models, modules, generic values, owned objects,
  interfaces, transactions, tasks and cancellation under explicit v1/v2/v3 protocols.
- `pipelang-model-viewc`: typed model/view bindings and resolved authored styles.
- `pipelang-native-cpp`: the active `pipelang.cpp.float64-calls.v1` scalar frontend,
  using an explicitly selected, checksum-pinned native emitter.
- `pipelang-sdk-tool`: manifest verification, bindings, execution audits and
  deterministic SDK packaging/extraction.

Unused historical profiles, Go APIs, stream/research frontends and old Python
orchestration have been retired. Their existence does not imply native support.
Useful language/JSON fixtures and C++ runtime tests remain. The migration record
is [here](docs/agents/tasks/native-toolchain-20261030.md).

## Build, test and package

The default qualified development path requires Linux, cgroup v2, a working
systemd user manager, CMake 3.21.1+, a C++20 compiler, POSIX shell and `strace`.
The first build compiles and independently qualifies native containment from
source inside a capped systemd unit; it needs no prebuilt PipeLang SDK.

Use the exact DockPipe version in `dockpipe.config.json` and `dockpipe.lock.json`.
If needed, install that checksum-locked binary into a fresh user-selected directory:

```sh
cmake -DOUTPUT_DIRECTORY=/absolute/tools/dockpipe -P tooling/bootstrap-dockpipe.cmake
export DOCKPIPE_BIN=/absolute/tools/dockpipe/dockpipe
```

Then run the default workflows, with no native feature flags:

```sh
"$DOCKPIPE_BIN" --workflow-file workflows/build/config.yml
"$DOCKPIPE_BIN" --workflow-file workflows/check/config.yml
"$DOCKPIPE_BIN" --workflow-file workflows/test/config.yml
"$DOCKPIPE_BIN" --workflow-file workflows/release/config.yml
```

Build/test/package outputs use fresh directories outside the checkout.
`bin/native-current` points to the last successful build; its `sdk/` contains the
installed compiler tools, runtime headers and exact `PipeLangModel 0.1.0` CMake
package. `bin/native-package` points to the last successful SDK archive and receipt.
The release workflow creates and relocates a local SDK candidate. It does not
publish, sign or promote a release.

C++ tests cover frozen configuration digests, diagnostics/events, SDK admission,
archive extraction, scalar metadata and IEEE selected values, ordinary-model
semantics, concurrency, objects, inheritance, view/style generation and runtime
sanitizers. This does not qualify every historical language profile or native UI.
Windows/macOS containment and release qualification remain outstanding. CI
configuration is provided; local execution is distinct from hosted proof.

See [validation](docs/agents/core/validation.md) for limits and selected proof paths,
and [the language direction](docs/agents/backlog.md) for planned capabilities.
