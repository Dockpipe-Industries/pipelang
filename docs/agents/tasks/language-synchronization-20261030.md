# Source synchronization: Lock and SemaphoreSlim

Objective contract:
objective_id: pipelang-synchronization-20261030
state: completed
execution_skill: dorkpipe-objective-execution
execution_authority: explicit owner reply to implement lock and SemaphoreSlim
authorized_objective: C++-first scoped source locks, explicit semaphore permits, cancellable/bounded synchronous waits and owned asynchronous semaphore waiting with reliable teardown
done_when: semantic/refusal/concurrency/resource/cleanup/sanitizer/compatibility fixtures and root workflows pass; APIs/limits documented; unrelated bytes and Git anchors preserved
inherited_invariants: explicit native selection; default wire/header compatibility; owned values/shared handle restrictions; existing numeric/results/transaction/evaluation and cleanup budgets; owner-only continuations and bounded task capacity
explicit_exclusions: commit/push/publication; consumer porting; MesaMoto/ForgePipe edits; DockPipe repository access; system installation; live UI; arbitrary Monitor/object locks; source disposal framework; Task escape; .NET ABI/full API compatibility; full compiler campaign restart
checkpoint_policy: automatic_within_objective
verification_policy: sequential containedexec job.py/run.py;1GiB/unit,800MiBproactive,zeroSwap,2GiBaggregate,8GiBnewestate; retain partial/failed proofs
handoff_policy: user_requested_only
context_pressure_policy: warn_and_continue
checkpoint_output_policy: quiet_success_bounded_failure
terminal_conditions: completed | blocked | failed_verification | cancelled

Baseline: 973 accepted authored PipeLang files on original
codex/pipelang-editor-staging HEAD33fb61ca74fe94748ecd6f66b562f7b5fd53da1a,
no staged paths; MesaMoto142/ForgePipe259 authored bytes unchanged. Prior concurrent
collection acceptance is the admitted baseline. No new publication is authorized.

## Selected contract

Lock and SemaphoreSlim are dedicated nongeneric shared-lifetime handles: private
fields fixed after construction, explicit model parameters/locals, not owned
records/containers/results/returns/public snapshots/host transport. Pure code
cannot construct or observe them. Ordinary parallel captures/model state remain
read-only even inside lock; use shared Atomic/concurrent collections for writes.

`Lock.Create()` or default creates a reentrant cell. `lock (gate) { ... }` owns one
evaluated handle and an RAII guard; release on normal exit, return, loop transfers,
source faults, cancellation and evaluation exhaustion. No suspension, nested
parallel launch or blocking semaphore wait inside a lock or transitive helpers.
Acquisition retries consume normal/cleanup tickets with cooperative cancellation;
cleanup acquisition uses its reserve. No fairness or global lock-order guarantee.

SemaphoreSlim default initializes0/max4096; Create(initial[,maximum]) validates
0<=initial<=maximum,1<=maximum<=4096. CurrentCount is observational, Release([count])
returns previous count and checks overflow before mutation. No ownership affinity;
release is explicit, so successful acquisition remains caller responsibility.
Several operations do not form an implicit transaction and ordinary model rollback
does not undo permit operations. No automatic permit lease/disposal is implied.

Wait()/Wait(token) return void; Wait(timeoutMilliseconds[,token]) returns bool.
Timeout0 is nonblocking, -1 has no wall-clock deadline, finite bounds0..60000.
Synchronous retrying waits consume evaluation tickets; cancellation throws typed
OperationCancelled before acquisition. Blocking Wait cannot run from async call
closures, lock closures or finally/cleanup closures. Parallel worker Wait is allowed.

WaitAsync has corresponding overloads and must be directly awaited. Non-timed
`await semaphore.WaitAsync(token);` is a void statement; timed await produces bool.
Cancellation is typed Cancelled, timeout is false, successful acquisition remains
authoritative if cancellation arrives later. Source catch(Error) cannot intercept
cancellation. Release remains explicit; callers use try/finally around protected
work and must not assume cancellation automatically returns an acquired permit.

Async waiting registers bounded owner-polled readiness, without hostworker blocking
or a new thread per wait. Continuations run only on owner poll; one entrysnapshot
per poll bounds delivery. Pending waits retain shared handles and unregister on
frame destruction; closing cancels without consuming new permits and drains source
cleanup before frame/state release. Already produced host outcomes remain
authoritative. Actual suspension resets segment budgets; immediate/timeout0/closing
completion does not. Task capacity/queue ceilings remain fixed32.

## Completed focused acceptance

The selected synchronization contract is implemented and documented in
[the canonical API guide](../../concepts/pipelang-synchronization.md). Final source-
built SDK qualification passed 83 sequential contained units and fifteen sanitizer
fixture pairs, including two new AddressSanitizer/UndefinedBehaviorSanitizer pairs
and forty synchronization refusal cases. Root check, build and test workflows
passed using installed DockPipe0.6.33 and the admitted Go toolchain. Existing model
header bytes remain identical. Existing owned/Atomic/concurrent runtime bytes and
CancellationToken/Task/Coroutine definitions remain unchanged; TaskScope adds the
bounded owner-polled readiness mechanism.

Proof covers scoped/reentrant lock release across control transfers and faults,
checked permits, synchronous ticket bounds, async cancellation/timeouts and grant
authority, host-worker progress, owner-only bounded poll delivery, stale-frame
unregistration, teardown cleanup, refreshed exceptional state and actual-suspension
budget boundaries. Release makes a permit available; a pending async acquisition
commits during owner readiness polling. Cancellation before that point leaves the
permit untouched. Semaphore release remains explicit after successful acquisition.

The first focused source generation exposed a nongeneric type/factory parsing
boundary; the qualified-name loop now leaves Lock.Create/SemaphoreSlim.Create as
member calls. The failed/partial receipt and artifacts are retained. ThreadSanitizer
was not retried because the preceding host probe rejected its runtime memory
mapping; no race-sanitizer, performance or release qualification is claimed.

Durable acceptance: `/tmp/pipelang-synchronization-20261030/acceptance.json`.
Exact source SDK proof: `/tmp/pipelang-synchronization-20261030/final-native/acceptance.json`.
MesaMoto/ForgePipe and unrelated PipeLang authored bytes, original HEAD/branch/index
anchors and all partial proofs remain preserved. Consumer porting is paused. No
commit, push, publication, system installation or live UI startup was performed.
Arbitrary Monitor/object locks, general disposal, automatic semaphore leases,
Task escape and complete .NET API/platform qualification remain deferred.
