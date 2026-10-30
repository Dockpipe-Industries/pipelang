# PipeLang

PipeLang is an independent typed language and SDK. Its compiler emits inspectable
workflow YAML and binding metadata for [Dockpipe](https://github.com/Dockpipe-Industries/dockpipe),
and provides deterministic language, application IR and experimental native code
backends. [Dockpipe packages](https://github.com/Dockpipe-Industries/dockpipe-packages)
contain real consumer examples.

Install the standalone CLI with Go:

```sh
go install github.com/Dockpipe-Industries/pipelang/src/cmd/pipelang@latest
pipelang check --in config.pipe
pipelang compile --in config.pipe --out out
pipelang invoke --in config.pipe --method FullName --format json
pipelang materialize --workdir . --from workflows
```

`materialize` accepts explicit roots or reads `compile.workflows` from
`dockpipe.config.json`. Output defaults retain the Dockpipe interoperability
contract under `bin/.dockpipe/pipelang`. The optional `DOCKPIPE_EVENT_LOG` JSONL
output follows `dockpipe.operation_event.v1` without loading the Dockpipe engine.

Go consumers import:

- `github.com/Dockpipe-Industries/pipelang/src/lib/pipelang` for parsing,
  analysis, compilation, invocation and semantic projection.
- `github.com/Dockpipe-Industries/pipelang/sdk/materialize` for source discovery
  and batch materialization.
- `github.com/Dockpipe-Industries/pipelang/src/lib/applicationir` for application IR.

The `pipelang-streamc` and `pipelang-native-cpp` commands are separate experimental
backend entrypoints. See [language contracts](docs/concepts/pipelang.md),
[language task records](docs/agents/tasks/pipelang-reactive-application-language/index.yaml) and
[contained verification](tests/containedexec/README.md) for their acceptance bounds.

Build and run baseline tests with an installed Dockpipe:

```sh
export DOCKPIPE_BIN=/absolute/path/to/dockpipe
"$DOCKPIPE_BIN" --workflow-file workflows/check/config.yml --
"$DOCKPIPE_BIN" --workflow-file workflows/build/config.yml --
"$DOCKPIPE_BIN" --workflow-file workflows/test/config.yml --
```

The baseline workflow covers standalone CLI, public materializer, frozen legacy
compatibility and focused compiler checks. It does not claim the full compiler
conformance or memory campaign. Generated-code and resource tests require the
verified cgroup harness documented in `tests/containedexec/README.md`.

Legacy package integration tests are retained in `legacy-integration-tests` as
migration provenance. Current package integrations belong in the packages
repository and consume this public SDK. Compatibility fixtures preserve both the
frozen legacy corpus and the authored models at extraction.

## Distribution

The Go module is the versioned compiler and SDK distribution channel. The `release.build` Dockpipe workflow produces checksum-manifested CLI archives for Linux, macOS and Windows, including the stream and native C++ compiler frontends. GitHub Actions retains the artifacts for review. An explicitly published version tag also creates a draft GitHub Release with those archives and checksums; publishing that draft remains an operator action. Homebrew and OS package registry publication remain follow-up work in the backlog.

Dockpipe is pinned with `dockpipe_version` in `dockpipe.config.json`.
`dockpipe.lock.json` records the exact release archives and SHA-256 checksums.
Run `python3 tooling/bootstrap_dockpipe.py --output /your/tool-directory` to install
that version; CI uses the same lock. Update the config and lock together.
The controlled toolchain uses the released Dockpipe 0.6.33 CLI. Each platform
archive and extracted executable size is verified against the lock. The lock
adds no cache service.


## Experimental C++ emitter port

The parser, type checker, HIR/Core lowering and default emitter remain Go. The
ported C++17 emitter consumes an internal, bounded admitted-Core protocol after
the same authoritative validation. It is explicitly selected and does not widen
the `pipelang.cpp.pilot.v1` profile or establish a public C++ ABI.

The build workflow enables the native target explicitly and requires an existing
C++17/CMake toolchain. Run it with an installed CLI admitted by the project config:

```sh
DOCKPIPE_BIN=/usr/bin/dockpipe PIPELANG_BUILD_CPP_EMITTER=1 \
  /usr/bin/dockpipe --workflow-file workflows/build/config.yml
bin/pipelang-native-cpp --source /absolute/Counter.pipe --output-dir /absolute/generated \
  --package forgepipe.ui.demo --module app.root \
  --cpp-emitter /absolute/pipelang-cpp-emitter --cpp-emitter-sha256 EXACT_LOWERCASE_SHA256
```

Omitting the emitter flags preserves the Go reference path. Supplying either
requires both; identity, protocol, unsupported-input and execution failures do
not fall back. Native output includes the emitter executable digest alongside
the frontend and generated-code identities. The existing runtime support header
is embedded from its single owner; the private JSON codec is pinned under
`src/native/third_party/nlohmann` with its MIT license and provenance.

For equivalence, run the existing contained native pilot exporter with
`PIPELANG_CPP_EMITTER` and `PIPELANG_CPP_EMITTER_SHA256`. Every exported program
must match Go source bytes and semantic bindings before native output is used
by the existing independent value/trace harness. Keep generated execution inside
`tests/containedexec/run.py`; baseline tests alone are not native conformance.
The ForgePipe installed UI example accepts the same explicit emitter selection
and validates its metadata independently.


Root workflows now use the verified 0.6.33 config and release lock directly,
without omitting the CLI gate. Historical port verification records retain their
original temporary-snapshot exceptions. Minimum-version consumer compatibility
remains a separate DockPipe contract.

The [port verification record](docs/research/pipelang-cpp-emitter-port.md) describes
the matched corpus, native UI evidence and remaining limits.


## Experimental C++ lexer slice

The next native compiler component is the lexer. Set `PIPELANG_BUILD_CPP_LEXER=1`
for the declared build workflow to produce `bin/pipelang-cpp-lexer`. The frontend
selects it only with both `--cpp-lexer /absolute/pipelang-cpp-lexer` and
`--cpp-lexer-sha256 EXACT_LOWERCASE_SHA256`. It can be combined with the existing
native emitter flags. Omitting either component's selection keeps its Go
implementation; a selected component failure never falls back.

`AnalyzeSemanticModuleSetWithLexer` accepts an explicit source-token function.
The native adapter implements that value boundary while the existing Go parser,
source admission, type checking, semantic IDs and lowering remain authoritative.
Token bytes, decoded text, EOF, lexical errors and source byte spans retain the Go
reference behavior, including its existing limited escape rules. Native lexing
is bounded to 64 KiB per source and does not expand the accepted language.

Native lexer tests compare complete token/failure values with `TokenizeSource`,
which wraps the unchanged Go lexer. Opt-in fixture and frozen-pilot comparisons
run inside the existing contained lane. The ForgePipe example pins the native
lexer separately and checks its identity in the generated metadata.

See [the lexer slice verification record](docs/research/pipelang-cpp-lexer-port.md) for acceptance evidence and limits.
