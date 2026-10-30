# Experimental native ordinary-model SDK

PipeLangModel 0.1.0 is an explicitly selected C++20 SDK for ordinary typed models
and compiled typed views. It is implemented; focused qualification evidence is
recorded by the owning remediation task. It does not replace the existing Go/Core
compiler, source-v1/v2 view profiles or pilot/comparison/conditional backends.

The native compiler owns class/record/enum typing, method effects, checked integer
arithmetic, transactions and structured host-service async/await lowering. The
native view compiler resolves public model fields and pure getters/actions into
typed adapters. Neither compiler recognizes application verbs such as Save/Revert.
The installed runtime owns bounded tasks, cancellation, creator-thread delivery
and teardown. The C++ host implements explicitly injected typed services only.

Build through the root's explicit optional workflow selection:

```sh
PIPELANG_BUILD_NATIVE_MODELS=1 DOCKPIPE_BIN=/usr/bin/dockpipe \
  /usr/bin/dockpipe --workflow-file workflows/build/config.yml
cmake --install bin/model-sdk-build --prefix /absolute/local/PipeLangModel
```

The SDK can also be built directly from this CMake project without Go or DockPipe;
repository qualification still runs native/generated execution inside the existing
containedexec job.py/run.py limits. Installation belongs in an explicitly selected
local artifact prefix; this does not authorize system installation or publication.

Consumers use `find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED)` and
`PipeLang::ModelRuntime`, then `pipelang_add_model_bindings` with MODEL, VIEW,
NAMESPACE and OUTPUT_DIRECTORY. Generated files are Model.hpp, model.json,
ModelView.hpp, view.json and bindings.json. The installed SDK manifest covers both
executables, runtime headers, CMake configuration, orchestration and notices.
Generation verifies the selected manifest, tools and sources before/after checked
native processes and publishes one complete output set. Existing outputs survive
refusals. No compiler discovery, latest-version lookup or Go fallback is performed.

Views bind ordinary model members, for example `bind model.draft.playerName` and
`bind model.CanSave()`, and reference typed methods such as `clicked: model.Save`.
Generated dispatch uses the reusable ForgePipe event-admission boundary and
projects values/enabled properties atomically. Inspection uses the last committed
projection, including its availability and explicit sensitive-value redaction.
Raw rendering state is not a sanitized inspection report.

The capability is bounded, not a claim of complete C#/Java coverage. It supports
owned records/enums and Result values, stateful classes, pure properties,
transactional synchronous helper composition and multiple sequential typed host
awaits. Local async-method composition, arbitrary generics, Optional, loops,
inheritance and exception/finally syntax remain unsupported. Those limits must not
be worked around with application opcodes or manually replicated model schemas.
The current consumer audit is closed at its recorded focused scope. Broader
language work requires a separately selected next capability and proof.

The native view compiler additionally supports explicitly selected, view-scoped
[typed styles](../../../docs/concepts/pipeview-styles.md): typed tokens, bounded
inheritance and explicit overrides lower to existing adapter style properties.
This does not add renderer dependencies to the compiler/runtime or change model
and action semantics. Unstyled source retains its prior generated artifacts.
