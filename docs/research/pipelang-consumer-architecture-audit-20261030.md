# Consumer architecture audit — 2026-10-30

Authority: the owner requested an audit of existing work against the language
direction, rather than accepting poor consumer code because a feature is missing.
Scope: existing PipeLang/ForgePipe counter, settings, typed UI and material
consumers. This records the original source/design audit and its completed remediation.
It does not qualify gameplay, native input, GPU presentation or the full compiler
suite. The original findings below are retained as audit evidence. Findings 1–4
are implemented and pass native semantic/lifecycle/sanitizer, installed-consumer
and terminal root compatibility checks. See the [remediation record](pipelang-native-models-20261030.md).
Finding 5 is addressed by retaining PercentStepper as one percent control without
adding specialized language constructs; general range/choice controls remain
planned only when required by a real consumer.

## Original findings and coherent remediation

1. **High: application actions are split across a closed UI operation dialect
   and handwritten native controllers.** `sdk/pipeview/state/model.go` defines
   Set/Save/Revert and limits the Save/Revert inventory; `sdk/pipeview/source/actions.go`
   lowers those operations into that private state machine. In the ForgePipe
   counter, `Counter.pipe` supplies scalar `CanIncrement` and `Reset` fragments,
   while `Example.cpp` owns reads, guards, checked arithmetic outcomes and state
   transitions. These fixtures prove a boundary, but are not sufficient typed
   application actions. Define a C++-first bridge for ordinary typed model methods,
   action payloads, state ownership and fallible outcomes. Migrate one complete
   Counter action, then settings; do not expand the enum with application verbs.
2. **High: compiler-known schemas are copied into application glue.**
   `examples/settings/Example.cpp` maps the three-field record into variant state,
   repeats its shape/types and reconstructs it from a save snapshot. It manually
   validates action/control identities, event kind, binding, control class and
   payload variants. ForgePipe's separate settings example repeats similar
   admission. Generate typed snapshots, action IDs/payload entrypoints and checked
   backend conversions from the admitted model. Put reusable event admission in
   the adapter owner and qualify both consumers against the same dispatch
   contract. Keep file-format migration and filesystem admission in SettingsStore.
3. **Medium: authored controls cannot bind existing model availability/status.**
   `sdk/pipeview/state/model.go` computes CanSave, but the source checker in
   `sdk/pipeview/source/source.go` allows Button only label/clicked (plus id).
   `examples/settings/SettingsView.pipe` cannot bind enabled state. Expected
   dispatch refusals are consumed after projection; detailed storage errors stay
   in the controller while the view exposes generic save.status. Add typed pure
   property/status/error bindings, sharing availability between rendering,
   dispatch and inspection. Do not fix this with more host-only strings or
   control-specific callbacks.
4. **Medium: floating helper-call refusal already duplicates material rules.**
   ForgePipe `src/MaterialAdmission.pipe` repeats the range/order expressions in
   ContactCoefficientsValid instead of composing WithinClosedRange and
   ContactOrderValid. The float profiles refuse calls. Define a reusable typed
   intra-program call capability with signature admission, deterministic argument
   evaluation, recursion/resource policy and independent native validation; then
   compose these methods. Avoid further aggregate predicates introduced solely
   to evade a missing call capability. MinimumRestitution itself is a clear pure
   expression and does not require a special language builtin or helper layer.
5. **Lower: keep PercentStepper a percent control, not a template for many
   specialized language constructs.** Its 0..100 domain is reasonable for volume.
   When a real consumer needs other numeric ranges or choices, define reusable
   typed range/step/choice properties instead of adding resolution/difficulty
   variants. No speculative replacement is needed now.

The first three findings blocked broader typed UI/application architecture
acceptance at audit time, even though the old bounded behavior tests passed. Prefer the typed
model/action bridge, then shared property/status binding. The material call
capability is a separate smaller compiler seam that removes current duplication.
Choose the next objective explicitly after the conditional-value checkpoint;
this audit is not authority to implement every planned language feature at once.

## Boundaries that should remain

ForgePipe owns material finite checks, JSON field/path/error order, optional
capabilities, solver calculations and transactional output publication. A
compiler capability does not absorb those responsibilities. SettingsStore owns
atomic I/O, format versions, symlink/file admission and recovery. Native workers
own creator-thread completion and teardown until general task semantics exist.
Exact executable hashes, signature admission and versioned transport are
reproducibility boundaries. Default and opt-in branches are necessary parity
fixtures while migration remains experimental.

For general tasks/async/parallel support, define ownership, cancellation, failure
propagation, cleanup, bounded concurrency and result ordering together. Existing
worker scaffolding is not a substitute for that language contract. Preserve
current receipts and refusal behavior while migrating each consumer.
