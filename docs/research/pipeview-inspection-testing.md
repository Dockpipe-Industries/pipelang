# PipeView inspection/testing slice

Status: locally implemented and verified, 2026-10-28. The previous async Save
checkpoint was complete, with no active duplicate work. This next bounded
continuation implements the first PV-04 inspection/testing contract and preserves
completed compiler/model experiments.

## Changes

Two optional SDK packages provide renderer-independent inspection and typed widget
handles. The inspection writer is `pipelang.pipeview.inspect.v1`, with default
binding-value redaction, explicit exposure, precision-safe int64 representation,
owned trees/flags and validated action/target/status wiring. The test host mounts
that declaration over the unchanged real reference model; typed handles dispatch
real actions, and callers control save completion explicitly.

The PipeLang-owned showcase adds a private native inspection adapter and a
BUILD_TESTING comparison fixture. Live capture requires the model owner thread.
The adapter reads authoritative draft/status, not presentation staging. Explicit
native proof mode writes redacted semantic files; ordinary startup has no
inspection endpoint or file export. Compiler, source profiles, state SDK, storage
and worker semantics do not change. Workflow testing gains public handle-type
checks; package graph checks now cover the new modules.

[The contract](../concepts/pipeview-inspection-testing.md) specifies API, privacy
scope and limits. Static labels/IDs/model status are public; redaction applies to
binding values and is not arbitrary-text secret detection.

## Evidence

Evidence root: `/tmp/pipeview-inspect`. Owning source hashes and temporary snapshot
exception are retained in `source-receipt.json`. Installed DockPipe validated the
owning test workflow and executed the temporary native inspection workflow in
bounded workers. Its repository and owning release pins/config are unchanged.

- `inspection-go.json`: tests default redaction, exposure, exact false/empty/text/
  int64 values, invalid source/model/policy combinations, ownership and deterministic
  JSON. Test-host cases compile the real SettingsView.pipe as their fixture and
  exercise typed widgets, Save/Revert policy, explicit outcomes, validation, loaded
  state, busy/stale/duplicate refusals and malformed wiring. Native parity is
  separately selected below rather than misreported from a skipped optional test.
- `dependencies.json`: neutral/state/source/inspection/testing module closures
  remain explicit; inspection/testing acquire neither compiler nor renderer in
  production imports. Ordinary compiler/materializer remain free of UI packages.
- `typed-handles.json`: a valid consumer compiles and three invalid public calls
  refuse: text passed to Toggle, bool passed to TextField, and a payload passed to
  Button.Invoke. The normal test workflow includes this regression check.
- `native-build.json`: both default-Go and selected-native component builds consume
  the qualified installed ForgePipe SDK. Generated validator/bridge/view-model
  headers match the prior async checkpoint exactly; no compiler/state campaign
  was repeated or promoted.
- `inspection-parity-{go,cpp}.json`: the real Session-driven native fixture matches
  the Go widget harness across initial, default-redacted, edited, pending Save,
  completed Save and reverted frames. Both reports are retained under
  `inspection-{go,cpp}`. The native fixture checks foreign-thread refusal, unknown
  exposure refusal and presentation-staging bypass. Fake persistence writes no
  settings file; the point is inspection conformance, not repeating storage proof.
- `inspection-json-{go,cpp}.json`: an independent report checker verifies exact
  fields, typed bool false, flags/availability, privacy and full semantic equality.
- `visible-inspection.json`: four visible native Vulkan runs cover pending and
  success/failure inspection files over real async persistence, while existing
  native model/worker/admission checks pass. All binding values are omitted in the
  semantic files, including the edited player name. PNGs still show actual UI
  values by design. This qualifies new capture integration, not physical input.

The SDK remains qualified ForgePipe0.9.0, archive SHA-256
`76b28415007dae63dd68f755825155451dda84b24ebd32be009e9004ff859f08`. Prior accepted
archive/expanded-file and model/compiler proof is reused; no sibling engine
implementation is compiled. Accepted receipts require no swap/OOM and completed
process-tree cleanup. `visible-result.json` records the visible result.

## Continuation

PV-04 remains broader than this slice: native action routing/endpoints, source
maps, semantic roles/label association, keyboard/focus/accessibility, richer
controls and broader backends remain pending. No general PipeLang test syntax,
network server, automatic publication or default unredacted export is claimed.
The installed renderer's visual enabled flag remains separate from action policy.
Go remains the inspection/model reference. Existing styles/XML/reload work and
settings cancellation/richer controls keep their own backlog acceptance.

No commit, push, publication, system installation, compiler binary update or
ForgePipe/DockPipe source edit is included.
