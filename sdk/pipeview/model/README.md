# Experimental native ordinary-model SDK

PipeLangModel 0.1.0 is the C++20 SDK for ordinary typed models and compiled
views. The default root workflows build, test and package it. Unused historical
Go/Core/source-v1/v2 implementations have been retired.

The native compiler owns class/record/enum typing, method effects, checked integer
arithmetic, transactions and structured host-service async/await lowering. The
native view compiler resolves public model fields and pure getters/actions into
typed adapters. Neither compiler recognizes application verbs such as Save/Revert.
The installed runtime owns bounded tasks, cancellation, creator-thread delivery
and teardown. The C++ host implements explicitly injected typed services only.

Build through the default root workflow, then consume the installed SDK under
`bin/native-current/sdk` with `find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED)`.
C++ containment and regression oracles are documented in
[validation](../../../docs/agents/core/validation.md).

No compiler discovery, latest-version lookup or Go fallback is performed.

Views bind ordinary model members, for example `bind model.draft.playerName` and
`bind model.CanSave()`, and reference typed methods such as `clicked: model.Save`.
Generated dispatch uses the reusable ForgePipe event-admission boundary and
projects values/enabled properties atomically. Inspection uses the last committed
projection, including its availability and explicit sensitive-value redaction.
Raw rendering state is not a sanitized inspection report.

The implemented headless capability includes owned records/enums, Result,
Optional and collection values; stateful classes; pure properties; transactional
helper composition; typed host and local async/await; lexical blocks/iteration;
and structured Error/throw/catch/finally handling. The
[composition and parallel contract](../../../docs/concepts/pipelang-native-model-composition.md)
adds explicit v2 source modules, exact overloads, closed generic records/methods,
joined parallel foreach, explicit shared Atomic cells, shared ConcurrentDictionary/
ConcurrentQueue/ConcurrentStack handles and common owned collections.
[Scoped lock and SemaphoreSlim](../../../docs/concepts/pipelang-synchronization.md)
add automatic lock release, explicit permits and cancellable owner-polled async
waiting without occupying the host-service worker. Request, specialization,
ownership and shared worker budgets remain bounded.

The [source-object capability](../../../docs/concepts/pipelang-objects.md) adds
explicit v3 model exports, ordinary reference-identity instances and source
Interface dispatch. Owner-confined graphs preserve alias identity, constructor
savepoints and shared-scope async calls. Records/public model snapshots remain
owned; native owner-bound reference results remain live handles. Model manifest
v2 is explicitly refused by the native view compiler and SDK view-binding helper
until independently qualified.

Generic classes/interfaces, constraints, inference, class inheritance, source resource
disposal, asynchronous finally and broader task orchestration remain unsupported.
The optional view bridge explicitly refuses modules, specialized generics and
overloaded methods/constructors until independently qualified. Existing consumer
evidence remains limited to its recorded scope; this is not complete .NET API
compatibility or cross-platform release qualification.

The native view compiler additionally supports explicitly selected, view-scoped
[typed styles](../../../docs/concepts/pipeview-styles.md): typed tokens, bounded
inheritance and explicit overrides lower to existing adapter style properties.
This does not add renderer dependencies to the compiler/runtime or change model
and action semantics. Unstyled source retains its prior generated artifacts.

`STYLE_INSPECTION` on `pipelang_add_model_bindings` opts into the versioned
[authored style descriptor](../../../docs/concepts/pipeview-style-inspection.md).
It adds an atomically published `style.json` covered by the binding receipt.
Default generation still owns exactly five outputs. The descriptor contains
resolved authored values; it is not an effective runtime-style report.

## Headless model generation

Engine and domain code can compile an ordinary model without authoring a view:

```cmake
find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED)
pipelang_add_model(contact-policy
    MODEL "${CMAKE_CURRENT_SOURCE_DIR}/ContactPolicy.pipe"
    NAMESPACE contact_policy
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/contact")
target_link_libraries(engine PRIVATE contact-policy)
```

This generates `Model.hpp`, `model.json` and `bindings.json`. It does not compile
or generate a view, discover Python, or introduce a renderer/UI dependency. The
native `pipelang-sdk-tool model-bindings` command verifies the selected model
compiler against the exact installed SDK manifest before and after compilation,
then publishes owned outputs atomically. Failed compilation preserves the prior
published directory; unowned files cause refusal. Receipts use
`pipelang.native-model-artifacts.v1` and contain only the selected model source,
compiler and generated file hashes.

The existing `pipelang_add_model_bindings` interface continues to require a model
and view. Types unsupported by the view bridge, including binary32, remain
explicit refusals there. A controlled consumer must select and verify its exact
SDK identity; the package version alone does not authorize mutable SDK selection.
