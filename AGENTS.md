# PipeLang

This repository owns the PipeLang compiler, SDKs, command line tools and language
conformance fixtures. It imports no DockPipe implementation source. Consumers use the installed,
versioned native compiler and SDK.

This repository must contain zero Go/Python source, scripts, manifests and
toolchain dependencies. Implement active compiler/CLI/SDK contracts in C++ and
run development operations through DockPipe. Unused historical
language profiles, Go APIs and research harnesses may be removed; do not recreate
them solely because they exist. Preserve inherited uncommitted work in a verified
snapshot outside the checkout before retirement. Workflow-specific scripts belong
beside their workflow in `workflows/<name>/assets/scripts`.

New explicitly selected ordinary-model semantics live in
`src/native/model-compiler`; typed native view compilation lives in
`src/native/model-view-compiler`. The owned-value/task runtime and exact installed
SDK belong to `sdk/pipeview/model`. Keep headless model/runtime consumers free of
ForgePipe/Qt dependencies; host-specific generated view bridges remain optional.
Preserve frozen regression inputs; retired historical profiles are not executable support promises.

Use `DOCKPIPE_BIN=/absolute/path/to/dockpipe` and invoke `workflows/check`,
`workflows/build` and `workflows/test` through explicit `--workflow-file` paths.
These run repository hygiene and bounded baseline checks. The default workflows use the independently qualified C++ `pipelang-contained` job/unit runner;
use it through the declared workflows as documented in validation guidance.
Keep compiler tests and resource campaigns within the unchanged verified caps.
The old harness is retirement input, not a permanent dependency; do not
add Go or Python build/test/packaging implementations. Never claim full-suite
coverage from a selected native subset. Preserve frozen compatibility goldens.

PipeLang's direction is a modern object-oriented language with familiar C#/Java
readability and expressiveness for humans and AI. Read the language direction in
`docs/agents/backlog.md` before designing consumer-driven capabilities. Mature C#
features are design references, not a requirement to clone every feature. When a
missing feature would materially worsen real consumer code, implement a coherent,
broadly useful capability instead of accumulating special cases, duplication,
manual unrolling or awkward helpers. Bounded slices are verification steps, not
the destination or an excuse for poor application architecture.

Implement new compiler/runtime capabilities in C++ first; avoid new Go
implementations that merely create future porting work. Retired Go code is not a build or correctness dependency. Make shared
semantic/IR changes deliberately and prove behavior independently. Preserve compatibility, resource limits and explicit
selection contracts, and label planned support separately from implemented support.
AI-first means clear semantics, strong types, explicit effects/failures, useful
diagnostics and inspectable behavior while preserving readable application code.
As async/parallel needs arise, define task ownership, cancellation, failure
propagation, cleanup, bounded concurrency and result ordering coherently.

Use descriptive names, ordinary formatting, clear failure propagation and bounded
resources. Do not introduce engine implementation code or sibling checkout paths.

Route tasks through `docs/agents/index.yaml`. Read focused architecture,
validation and workflow guidance as needed. `docs/agents/backlog.md` records the
gradual C++ migration direction and outstanding publication/qualification work.
