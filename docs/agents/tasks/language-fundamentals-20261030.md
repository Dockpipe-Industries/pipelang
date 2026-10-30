# Language fundamentals before consumer porting

Objective contract:
objective_id: pipelang-language-fundamentals-first-20261030
state: completed
execution_skill: dorkpipe-objective-execution
execution_authority: explicit owner instruction, continued by requested handoff
authorized_objective: establish and implement the language foundation required by actual material and ordinary-model code before more consumer porting
done_when: required foundation has coherent C++-first implementations, independent semantic/refusal/resource proof, retained compatibility and an explicit consumer-readiness gate
inherited_invariants: ownership, explicit backend selection, typed Failure/Cancelled, frozen defaults, IEEE behavior, preserved unrelated bytes
explicit_exclusions: MesaMoto changes; DockPipe repository access; consumer porting/UI expansion; new commits, pushes, publication, system installation, credentials, cost, native UI startup; full compiler campaign restart
checkpoint_policy: automatic_within_objective
verification_policy: sequential job.py/run.py units; 1 GiB hard, 800 MiB proactive, zero swap, 2 GiB aggregate; fresh estate <=8 GiB; retain failed proof
handoff_policy: user_requested_only
context_pressure_policy: warn_and_continue
checkpoint_output_policy: quiet_success_bounded_failure
terminal_conditions: completed | blocked | failed_verification | cancelled

## Admitted baseline

Handoff anchors were checked on 2026-10-30: all 142 MesaMoto, 929 PipeLang and
259 ForgePipe authored files match; HEAD and complete porcelain status hashes
match. Existing acceptance receipts for native model remediation, styles and style
inspection remain admitted; their suites are not rerun to rediscover history.

## Foundation readiness checkpoint

The C++ ordinary model compiler supports brace-bodied methods, typed locals,
if/else/early returns and synchronous helper composition. It admits int64,
bool, strings, records, enums and owned Results; integer arithmetic is checked.
It does not admit float at all. Bare nested blocks are also absent despite the
README's broader wording. Existing host awaits are sequential, owned and bounded;
local async composition, iteration, exceptions/finally and general cleanup syntax
are absent. Do not conflate host awaits with a general async language.

MaterialAdmission uses an independently selected binary64 helper-call expression
profile, reached through the legacy frontend/Core. Its five predicates are
qualified; this is not full function-body support for material code. The model
compiler's bodies cannot currently consume those float signatures.

Actual PhysicalMaterial/MaterialContactInteraction code needs binary64 arithmetic,
finite guards, structured validation/early return, owned values and error paths.
PhysicalMaterial's definitions/fields/extensions and MaterialExchangeLedger's
receipts/components use iteration. Copying their loop bodies into unrolled source
would be unacceptable. SettingsModel requires source-owned async orchestration,
Result propagation, captured snapshots and cancellation; extracting async helpers
must preserve transaction visibility and task teardown rather than starting
independent actions or worker threads.

## Implementation order and gate

1. Unify ordinary value/function fundamentals in the explicit C++ model path:
   binary64 literals/values/arithmetic/comparison plus real lexical blocks,
   preserving strict types, left-to-right evaluation, lazy arms and IEEE values.
2. Define bounded general iteration over owned data, with an explicit resource
   policy covering nested loops and helpers; qualify branch/local/return behavior.
3. Define local async composition with shared task ownership, cancellation,
   transaction boundaries and cleanup. Evaluate structured cleanup from actual
   owned-resource requirements; do not add exception syntax without its contract.
4. Independently verify required source semantics, refusal/resource boundaries and
   unchanged legacy artifacts. Record precisely which paths are qualified.

Consumer porting remains CLOSED until all prerequisites for the selected next
consumer responsibility are independently accepted. Completing a numeric or block
slice does not open this gate. Collection iteration and local async/cleanup remain
explicit outstanding requirements until implemented and qualified. This document
is an execution checkpoint, not a completion claim or authority to port consumers.

## Implemented checkpoints

The initial inventory above describes the admitted baseline, not the final
capability. The explicit native model path now implements binary64 values,
exponent literals, arithmetic/comparisons and IsFinite/Abs/Sqrt/Log, owned Optional
and List composition, lexical blocks, while/foreach/break/continue, bounded
iteration and local async helper composition. No application/engine code changed.
The model compiler README is the canonical grammar/semantics contract.

`value-proof` and `iteration-proof` passed focused generated execution, refusals,
installed native SDK/tool admission and ASAN. `composition-proof` retained an
unused generated cancellation-parameter failure; `composition-proof-2` passed
nested async behavior and sanitizer proof after repair. Independent review also
caught and repaired numeric use confined to field defaults, immediate closing
await budget resets and explicit-token inheritance to tokenless descendants.
Direct regression fixtures cover those conditions. The sandbox could not access
the user bus on the first preflight; the authorized existing systemd harness ran
with escalation, without changing containment or machine configuration.

## Cleanup and remaining language boundary

Required cleanup for these owned-value/host-service models is deterministic frame,
argument, transaction, service and task-scope ownership. Root teardown joins work
and drains child/parent outcomes before releasing state/services. Source receives
Success/Cancelled/Failure and can compose its normal outcome handling. Runtime
faults roll back the current segment. An intentional returned Failure/Cancelled
commits authored changes. There is no source-owned disposable external resource.

Therefore this objective does not add speculative try/catch/finally or disposal
syntax. Those remain explicit prerequisites before porting a responsibility that
owns external resources or requires asynchronous cleanup. Inheritance, overloads,
arbitrary generics/references, parallel task orchestration, Task escape, recursive
JSON trees and broad codec facilities also remain unsupported. This is a deliberate
readiness boundary, not a promise that those constructs exist.

## Consumer resumption gate

No consumer porting is performed by this objective. The existing material
expression profile and all default pins remain unchanged. The next eligible
responsibility is typed owned material-value validation using the qualified
native model path; it must retain finite checks, optional absence, first-error
ordering, IEEE defaults and transactional outputs. Its actual adapter/source/SDK
admission and behavioral equivalence are a separate consumer acceptance checkpoint.

The foundation gate is ACCEPTED for the explicitly selected C++ owned-value/model
contract after independent semantic/refusal fixtures, a separate code review,
contained generated execution, ASAN and terminal compatibility checks. Scoped
consumer qualification may now be assessed against that contract; this objective
does not start a port or change any consumer dependency/profile selection. Codec/catalog traversal needing
recursive JSON, new host resources, disposal, async cleanup, parallel tasks or
other missing semantics stays closed until those fundamentals are implemented
and independently accepted. Do not unroll, duplicate logic or add consumer opcodes.

## Terminal acceptance

State: completed for the defined foundation contract, 2026-10-30.

- 31 sequential native SDK/compiler/generated-execution units passed, including
  four semantic/lifecycle ASAN runs, 50 new matrix refusal cases, additional
  parser/literal bounds, strict IEEE compilation-mode refusal, existing model/view
  regressions and 15 installed SDK-tool admission tests.
- Root check/build/test passed using `/usr/bin/dockpipe` 0.6.33 and installed
  Go 1.25.13. The first build used an older system Go; its failure is retained,
  and the corrected build/test receipts pass. No toolchain was installed.
- The existing ordinary-model generated header is byte-identical to the initial
  retained baseline. Legacy lexer mode, Go/Core profiles and their frozen
  compatibility fixtures pass unchanged.
- Exact installed private SDK, fixture/source hashes, complete unit/job teardown,
  memory/swap counters and before/after inventories are retained in
  `/tmp/pipelang-fundamentals-20261030/acceptance.json` and its referenced receipts.
  Limits remained 1 GiB/unit, 800 MiB proactive, zero swap, 2 GiB aggregate and
  8 GiB fresh estate. Partial/failed attempts were preserved.
- MesaMoto and ForgePipe authored bytes and original Git anchors are unchanged;
  PipeLang changes are limited to compiler/runtime, independent fixtures,
  qualification tooling and canonical capability/readiness documentation.
- No new commit, publication, consumer port, native UI startup or system install.
  Full compiler conformance, non-Linux platforms, interactive gameplay, performance
  and release qualification remain outside this focused acceptance.
