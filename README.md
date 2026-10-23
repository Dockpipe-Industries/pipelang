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
This initial pin uses the published 0.6.25 staging release because the stable
release predates the required workflow features. The lock adds no cache service.
