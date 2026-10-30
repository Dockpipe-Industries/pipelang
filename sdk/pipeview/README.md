# Experimental PipeView declaration SDK

The declaration SDK now has an optional bounded PipeLang source frontend,
selected through `pipelang-viewc`. It is experimental and does not implement a
general reactive language runtime or a production UI framework. The bounded
typed state/action profile is described [here](../../docs/concepts/pipeview-state-actions.md).
Go remains the compiler reference. See the [source profile](../../docs/concepts/pipeview-source.md).

## Packages

- `github.com/Dockpipe-Industries/pipelang/sdk/pipeview` owns immutable declarations,
  bool/int64/UTF-8 text bindings, typed UI event requirements, stable identities
  and inspection snapshots. Its dependencies are standard library only.
- `github.com/Dockpipe-Industries/pipelang/sdk/pipeview/forgepipe` is an explicitly
  selected projection to `forgepipe.ui.v1`, or explicit `forgepipe.ui.v2` when a
  PercentStepper requires that capability. It depends on
  the declaration SDK and standard library only; it imports no compiler, engine
  or Qt implementation. Native rendering remains ForgePipe-owned.
- `github.com/Dockpipe-Industries/pipelang/sdk/pipeview/source` owns the optional
  source component/type checker. It depends on the existing compiler, neutral
  SDK and state contract; it has no renderer or CLI dependency. Syntax remains compiler-owned.
- `examples/pipeview-forgepipe` is the original Go SDK authoring example, not
  a general compiler or package resolver. It remains useful as a host SDK example.
  ForgePipe's UI consumers now use
  `.pipe` view sources through `pipelang-viewc`; separate `.pipe` classes own
  their compiled application logic.

These packages use the repository's existing Go module/version mechanism. They
are experimental public Go APIs, not newly accepted PipeLang import names or an
independent package registry. Consumers pin the owning module revision/release;
there is no runtime discovery, implicit backend selection or sibling fallback.
Distribution, native ABI compatibility and authored PipeLang package imports are
separate acceptance work. No empty Qt, markup, styles or testing packages have
been created ahead of their implementations.

The optional `sdk/pipeview/state` package owns the Go reference draft/saved model,
closed typed actions, captured save requests and terminal outcomes. Its dependency
closure is the neutral SDK plus standard library only. The C++ companion is
embedded by the explicitly selected action-profile exporter and checked against
Go traces. The state package owns no compiler, renderer, filesystem or scheduler.

## Typed construction

```go
updates := pipeview.State("settings.updates", true)
document, err := pipeview.Compile(pipeview.Column("settings",
    pipeview.Toggle("settings.updates.control", "Enable updates", updates,
        pipeview.Change[bool]("settings.updates.changed")),
    pipeview.Button("settings.reset", "Reset", pipeview.Click("settings.reset.clicked")),
))
```

A string binding or string change event cannot be passed to `Toggle` in Go.
`Compile` additionally refuses invalid/duplicate control identities, incompatible
shared-state/event declarations, zero bindings/events, malformed UTF-8 and bounded
capacity failures. Identical repeated binding/event declarations are allowed.
`Inspect` returns an owned copy with control kinds, text, binding identities,
typed initial values and typed event targets. Modifying that snapshot does not
change the document. This is declaration inspection, not the planned live AI or
accessibility protocol, and it contains application data: hosts must decide what
is safe to expose.

A UI event identity is not a canonical PipeLang method identity or permission to
execute it. The host bridge remains responsible for checked compiler metadata,
capability admission and dispatch. No action, task, state mutation or host process
is executed by the core package. ForgePipe's prototype Session applies toggle and
text edits before emitting events; this package does not change that behavior or
claim checked-action state semantics for the declaration package. The separate
state package supplies authoritative action-managed state in the new showcase.

## Limits and adapters

The neutral profile permits 128 nodes, eight levels, 128-byte ASCII identities
and 4096-byte UTF-8 text values. The ForgePipe adapter deliberately refuses the
narrower backend limits: identities over 64 bytes and documents over 65536 bytes.
It preserves int64 values without floating-point conversion. Its explicit scale
and focus-color options select the existing legacy renderer style profile; they
are not PipeView styles. New controls or styling features need declared backend
capabilities and focused proof, not a silent approximation.

Build the optional example with `PIPELANG_BUILD_PIPEVIEW_EXAMPLE=1` through the
existing build workflow using installed DockPipe. The ForgePipe UI example now selects `pipelang-viewc` through
an absolute `PIPEVIEW_EXAMPLE_GENERATOR` path and its SHA-256 in addition to its
existing compiler pins. The generator is local development tooling; it is not
installed system-wide by the workflow.

See the [roadmap](../../docs/agents/backlog.md#pipeview-modular-typed-ui-roadmap)
and [first-slice evidence](../../docs/research/pipeview-package-boundary.md).


## Semantic inspection and testing

Optional `sdk/pipeview/inspect` captures an owned, value-redacted semantic frame;
`sdk/pipeview/testing` supplies typed stable-ID widget handles over the real state
model. They do not import a compiler/renderer or own I/O/processes/clocks. They
require an admitted managed state/action declaration. See the
[contract](../../docs/concepts/pipeview-inspection-testing.md) for exposure policy,
explicit Save completion, limits and backend comparison evidence.


## Optional XML frontend

`sdk/pipeview/markup` converts the bounded declarative XML profile through the
same source checker and state plan. The compiler owns canonical PipeLang view
formatting. Neither path imports an engine. See the
[format contract](../../docs/concepts/pipeview-markup.md) for explicit syntax
selection, conversion receipts and semantic round-trip limits.

## Fixed percent editing

`PercentStepper(id, label, Binding[int64], Event[int64])` adds a fixed 0..100,
step-one editor. Declarations containing it identify declaration.v2 and require
Forge transport v2; unchanged declarations retain v1 bytes. Gesture bounds do
not constrain the model's int64 domain: invalid drafts remain visible and domain
validation blocks Save. The typed test handle accepts int64 through the same
set action. See the [bounded contract](../../docs/research/pipeview-percent-stepper.md).
