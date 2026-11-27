# PipeView semantic inspection and typed testing

Status: first bounded PV-04 contract implemented, 2026-10-28. This extends the
managed state/action profile, not ordinary language test syntax, OS accessibility
or a complete automation server. Go remains the reference; backend adapters must
match its versioned semantic projection.

## Packages and authority

- `sdk/pipeview/inspect` reads admitted neutral declarations and authoritative
  state snapshots. It imports only the core/state SDKs and standard library.
- `sdk/pipeview/testing` mounts that declaration over the real Go state model and
  offers typed stable-ID widget handles. It imports core/state/inspect and standard
  library, without compiler, renderer, filesystem, process or clock ownership.
- Source compilation is a separate caller step. Native adapters own capture on
  their model's owner thread and may hand an owned frame to other tools afterward.
  Neither inspecting metadata nor reading an action identity grants execution
  authority. No listener, remote action endpoint or background inspection service
  is introduced.

The first inspection contract is `pipelang.pipeview.inspect.v1`. A frame contains:

| Field | Meaning |
| --- | --- |
| `root` | Stable control IDs, component kinds, static text, bindings/actions and children |
| `bindings` | Canonical scalar type, read-only/redacted flags and optional current value |
| `actions` | Admitted operation, payload type and optional target state identity |
| `model` | Authoritative dirty/valid/saving/can_save flags and presentation status |

`enabled` describes the declaration, currently true for the neutral profile.
`action_available` is present only on interactive controls and describes current
model policy. A visually enabled button may have an unavailable action. They must
not be conflated until a backend supports dynamic visual disabling.

The inspector validates declaration/state/action/derived-status agreement and
snapshot consistency before projecting. It does not invoke an action, application
validator, renderer or persistence client. The snapshot's domain validity decision
is authoritative; inspection does not try to infer it from display text.

## Values and privacy

The zero policy hides every binding value, including derived status bindings.
Redacted entries contain type and flags but no `value` property. Explicit exposure
uses exact binding IDs; unknown IDs refuse, including false-valued unknown policy
keys. Exposed bools remain JSON booleans, strings retain exact UTF-8, and int64
values use decimal strings to avoid JavaScript-number precision loss. False and
empty values remain distinguishable from redaction.

Static labels, control/action identities and model status remain visible. This is
binding-value redaction, not automatic secret detection in arbitrary text. Saved
values, pending request snapshots, persistence errors and callbacks are not
exported. A trusted test host's direct Snapshot API still returns its owned full
state; publication should use the inspection policy, not that debugging accessor.

All frame collections are owned copies. Caller changes to a returned frame cannot
change the model/declaration or a subsequent frame. Inputs must be stable for the
capture call. Native inspection refuses calls from a foreign thread before reading
Application/Model/Session data.

## Typed testing

Compile source separately into a checked Program, then mount it. Supply the
application's deterministic validator to match domain rules; a nil validator only
uses the state model's default validity behavior.

```go
host, err := viewtest.Mount(program.Document, *program.Definition,
    program.StatusBindings, viewtest.Options{Validate: validateSettings})
if err != nil {
    t.Fatal(err)
}
name, err := host.TextField("name-field")
if err != nil {
    t.Fatal(err)
}
if err := name.Change("RiderX"); err != nil {
    t.Fatal(err)
}
save, err := host.Button("save-button")
if err != nil {
    t.Fatal(err)
}
request, err := save.Invoke()
if err != nil || request == nil {
    t.Fatalf("Save did not capture a request: %v", err)
}
if err := host.Complete(request.ID, state.Success); err != nil {
    t.Fatal(err)
}
frame, err := host.Inspect(inspect.Policy{}) // Values remain hidden.
```

TextField.Change accepts only string, Toggle.Change only bool,
PercentStepper.Change only int64, and Button.Invoke
no payload. Unknown IDs/wrong component kinds refuse; zero handles refuse; Host
must come from a successful Mount. Snapshot requires that mounted host. Methods
are serialized by the caller, with no test goroutine/scheduler supplied by the
package. Mount bounds and validates inputs before retaining owned copies.

Handles dispatch the declaration's real model action. They cannot directly set
raw state. Save returns an owned request; tests explicitly supply the typed
completion, so no arbitrary sleeps or real I/O are needed. A Revert button returns
no save request. Busy, invalid and clean-save policies are the real model's
refusals, with stale/duplicate completion behavior preserved.

## Native proof surface and limits

The PipeLang-owned settings showcase implements a private adapter over its live
owner-thread model and the installed ForgePipe document. It ignores transient
Session value staging when reporting authoritative bindings. An explicit proof
run writes redacted `inspection.json` and `pending-inspection.json` next to its
captures. Ordinary startup writes no inspection files and starts no endpoint.
Screenshots inherently show rendered values; semantic redaction does not redact
pixels. Exposed fixture reports are produced only by the BUILD_TESTING inspection
fixture with its explicit known-value policy.

This is a semantic/control harness, not a pointer/layout/focus/physical-input or
platform accessibility simulator. The native comparison fixture separately uses
real Session input. Richer controls, source maps, language-authored tests, broader
backend automation and hosted inspection require separate contracts/proof.

See [verification](../research/pipeview-inspection-testing.md) and the
[settings showcase](../../examples/settings/README.md).

Documents containing PercentStepper use `pipelang.pipeview.inspect.v2`; existing
frames retain inspect.v1. Value redaction and decimal-string int64 projection
are unchanged. The new typed handle submits the raw integer to the reference
model, including invalid domain values for validation tests; it does not emulate
native gesture clamping or invent a second state owner.
