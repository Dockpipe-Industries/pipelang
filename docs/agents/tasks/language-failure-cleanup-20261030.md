# Structured failure handling and cleanup

Objective contract:
objective_id: pipelang-language-fundamentals-first-20261030
state: completed
execution_skill: dorkpipe-objective-execution
execution_authority: explicit owner instruction to continue compiler work before further porting
authorized_objective: extend the accepted native foundation with coherent source failure handling and cleanup across ordinary and async control flow
done_when: owned Error, throw/rethrow, try/catch/finally and their transaction, cancellation, cleanup-fault and resource semantics have independent source/native/refusal/sanitizer proof; existing compatibility remains intact
inherited_invariants: explicit native selection; C++ first; owned values; typed Result success/cancel/failure; frozen defaults and IEEE behavior; prior unrelated bytes preserved
explicit_exclusions: consumer porting; MesaMoto/ForgePipe edits; DockPipe repository access; commits/push/publication; system install; credentials/cost; native UI startup; full compiler campaign restart
checkpoint_policy: automatic_within_objective
verification_policy: sequential existing containedexec job.py/run.py; 1 GiB/unit; 800 MiB proactive; zero swap; 2 GiB aggregate; <=8 GiB new estate; preserve failures
handoff_policy: user_requested_only
context_pressure_policy: warn_and_continue
checkpoint_output_policy: quiet_success_bounded_failure
terminal_conditions: completed | blocked | failed_verification | cancelled

## Admitted foundation

The completed 31-unit native/ASAN proof and root workflows in
`/tmp/pipelang-fundamentals-20261030/acceptance.json` remain admitted. HEAD is
33fb61ca74fe94748ecd6f66b562f7b5fd53da1a; no staged paths; 59 unstaged and 233
untracked authored paths. The 942-file final authored manifest is the byte baseline.

## Selected language contract

- `Error` is an owned value with validated, read-only `Message`; construct it with
  `Error(message)`. `throw Error(...)` raises a model execution fault. `throw;`
  rethrows the nearest lexically caught model Error. Only model execution faults
  enter `catch (Error error)`, not arbitrary native exceptions or evaluation-budget
  exhaustion. Per-value string/list capacity failures remain recoverable model
  faults; catching them cannot replenish either evaluation budget. Host/child Result Failure/Cancelled remain ordinary typed outcomes.
- `try` requires catch and/or finally. Catch recovery retains the working state
  preceding the fault; no implicit savepoint is introduced. Uncaught faults still
  roll back the last transaction segment. Returning Failure/Cancelled deliberately
  commits authored state, including successful cleanup.
- Try and catch may await in async methods. Catch lowers through an owned error
  value outside the native C++ handler, so it has no dangling exception reference
  or artificial native-handler suspension restriction.
- Finally runs once on normal completion, return, break/continue, fault and owner
  teardown. It sees enclosing locals/current transaction, not try/catch locals.
  It cannot return, await or transfer out of its scope; local loops may break or
  continue within themselves. Synchronous helpers and fallible expressions remain
  usable. Finally does not run merely because try/catch suspends.
- Return payloads are evaluated and owned before finally. Async return commits only
  after cleanup succeeds; a normal cleanup fault replaces a pending return. A
  cleanup fault during primary unwinding preserves the primary fault. Forced
  coroutine-frame abandonment suppresses cleanup faults rather than throwing
  through frame destruction. No source external-resource/disposal API is added.
- One shared bounded cleanup reserve (65,536 steps/segment) is distinct from the
  65,536 normal steps. Nested guards/helpers share it; guards do not replenish it.
  Evaluation-budget exhaustion is noncatchable by source Error handlers. Only actual
  host suspension resets segment budgets. Cleanup is attempted once under its
  reserve; exhaustion can prevent further authored effects, while native owned
  values and frames still release deterministically.

Consumer porting remains paused. Acceptance applies only to the explicit native
model path; legacy Go/Core/profile semantics are not silently widened.

## Implementation checkpoint

The C++ parser/checker now admits the selected syntax and tracks fallthrough,
return, throw and loop-transfer outcomes separately. Generator visitors and
static call/work bounds include catch and finally bodies. Catches retain owned
Error values outside native handlers, so authored recovery can await.

The runtime owns a bounded shared cleanup reserve, once-only cleanup guards,
secondary-fault precedence and frame-abandonment protection. Async return commit
happens after cleanup; coroutine results are staged privately until final
suspension. This prevents a recovered cleanup fault from exposing an earlier
return while its catch continuation is still pending.

`proof-1` passed 36 native units including five ASAN fixture pairs, the independent
21-case refusal matrix, shared reserves, transaction behavior, awaited recovery,
return/cleanup recovery, teardown and forced-frame-abandonment runtime tests.
A subsequent focused regression against that retained SDK also passed owned local
return/capture behavior, including a pure method. Normal ordered-expression
lowering already preserves the copy semantics; no extra return rewrite was needed.
Terminal proof after the final fixture/documentation changes is recorded below.

Source-owned external-resource handles/disposal, asynchronous finally, arbitrary
exception inheritance and parallel task orchestration remain unsupported. No
consumer work is included in this continuation.

A final ABI pass kept the existing ExecutionFailure base and Transaction::commit
signature unchanged: an explicit EvaluationLimitFailure handler rethrows before
source Error recovery, rather than adding a virtual classification method.
The terminal workflow coordinator encountered a temporary Go directory disappearing
during its storage inventory; no compiler test failure preceded the interruption.
The failed/partial receipt is retained, and the sampler now ignores only expected
ENOENT races while retaining all other inventory errors and the unchanged cap.

## Terminal acceptance

Completed for the selected explicit native failure/cleanup contract, 2026-10-30.

- Final `terminal-native-2` passed 36 sequential native units, including five
  semantic/lifecycle ASAN fixture pairs, the new 21-case source-refusal matrix,
  existing native/compiler/view/SDK regressions and direct runtime ownership tests.
- Qualified normal/return/loop/fault cleanup, shared reserves, primary-fault
  precedence, partial working-state recovery, uncaught rollback, awaited catch,
  delayed task-result publication, authoritative host success, cancellation and
  teardown. Owned local returns and pure cleanup reads retain copy semantics.
- Root check/build passed; the interrupted root test had no reported test failure
  before its inventory race. The repaired `terminal-workflows-2` test and its
  aggregate completed successfully with full process-tree removal. All attempts
  remain retained. No resource cap was raised and no proof was pruned.
- The existing generated model header, ExecutionFailure base class and complete
  Transaction definition remain byte-identical to the accepted foundation SDK.
  Go/Core/profile semantics and default dependency selections remain unchanged.
- Exact source/private installed SDK identities, receipts, resource counters and
  preservation inventories are indexed by
  `/tmp/pipelang-cleanup-20261030/acceptance.json`. The 1 GiB unit, 800 MiB proactive,
  zero-swap, 2 GiB aggregate and 8 GiB estate limits remain in force.
- MesaMoto and ForgePipe authored bytes, plus original HEAD/branch/index anchors,
  are preserved. No commit, push, publication, system install, consumer port or
  native UI startup occurred. Consumer porting remains paused by owner instruction.

Further compiler work may address source resource/disposal contracts, asynchronous
cleanup, broader type composition and bounded parallel tasks against real needs;
none is implied to be implemented by this acceptance. The full compiler campaign,
other platforms, performance, interactive and release qualification remain separate.
