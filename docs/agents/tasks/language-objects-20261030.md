# Object instances and source-defined interfaces

Objective contract:
objective_id: pipelang-objects-20261030
state: completed
execution_skill: dorkpipe-objective-execution
execution_authority: explicit user instruction to implement object instances and interfaces next
authorized_objective: C++-first owner-confined reference-identity class instances, source interface implementation/dispatch, composition and alias-aware transactional lifetime with existing async/task contracts
done_when: source semantic/refusal/identity/dispatch/rollback/constructor/lifetime/async/resource/sanitizer/compatibility proofs and root workflows pass; exact API/limits documented; unrelated bytes and Git anchors preserved
inherited_invariants: explicit native selection; old v1/v2 wire/header compatibility; existing owned/shared value rules, numeric/results/cancellation/cleanup/task ownership and budgets
explicit_exclusions: commits/push/publication; consumer porting; MesaMoto/ForgePipe edits; DockPipe repository access; system installation; live UI; class inheritance; generic classes/interfaces and instance generic dispatch; general downcasts/null; processglobal GC; frozen graph snapshots; source finalizers/disposal; complete .NET ABI/API; full compiler campaign restart
checkpoint_policy: automatic_within_objective
verification_policy: sequential existing containedexec job.py/run.py;1GiB/unit,800MiBproactive,zeroSwap,2GiBaggregate,8GiBnewestate; retain partial/failed proofs
handoff_policy: user_requested_only
context_pressure_policy: warn_and_continue
checkpoint_output_policy: quiet_success_bounded_failure
terminal_conditions: completed | blocked | failed_verification | cancelled

Baseline: accepted synchronization 980 authored PipeLang files at original
codex/pipelang-editor-staging HEAD33fb61ca74fe94748ecd6f66b562f7b5fd53da1a,
no staged paths; MesaMoto142 and ForgePipe259 unchanged. Prior acceptance remains
admitted evidence; no commit or publication is authorized for this continuation.

## Selected language and API contract

Object-enabled requests explicitly select pipelang.native-model.v3 and required
models:[rootClassNames], using either source or closed modules+entry plus optional
namespace. Old v1/v2 retain their contracts and reject new object constructs.
Only selected root models export host-facing state/scope wrappers; other classes
are ordinary source objects. A root export cannot be constructed as a source
object or implement a source interface in this slice. Exported snapshot fields
must remain recursively owned; reference-bearing fields are rejected explicitly,
never silently omitted. Live object results/parameters are owner-bound native
values, not serialized view transport. Object manifests use a separately versioned
schema; view generation refuses that schema until independently qualified.

`public source Interface` is an explicit language contract; old Interface stays
host-service injection. Class C : I,J supplies matching public methods with exact
parameter/return/pure/async contracts. `new C(args)` picks a checked constructor;
this, public/private fields and instance methods preserve identity. Implicit
concrete-to-implemented-interface upcasts retain identity. Exact overloads win;
unique interface-conversion candidates may follow; ambiguous calls refuse. No
structural interface guessing or implicit numeric conversion. Interface calls
use closed concrete target sets for call/effect/cycle/work analysis.

Class/interface references are non-null after successful construction. Use
Optional for absence. Assignment/arguments/returns share identity. References
can compose directly and through ordinary Optional/List/Array/Dictionary/Queue/
Stack/Result values, including List<I>. Records remain recursively owned and
cannot contain references. Shared/concurrent-cell payloads, hash/reference keys,
host-service transport, worker captures and exported snapshots cannot contain
live references, including references hidden in wrappers. Locks do not weaken
owner confinement. Source object methods may be synchronous or async; async calls
retain receiver/arguments in the existing root scope with no per-object worker.

## Runtime and transaction contract

Each root owns one thread-affine managed domain. Internal object fields contain
nonowning typed identity edges; locals/arguments/results use rooted leases. Weak
per-cell root caches plus bounded tracing reclaim unreachable cycles/transients;
a monotonic arena or shared_ptr ownership cycle is not acceptable. Closed-owner,
cross-domain and wrong-thread access fail deliberately. ObjectOwner closes only
after TaskScope joins/drains frames; escaped references detect the closed domain.
No source finalizers run during tracing.

Initial heap bounds:4096 live cells,16384 reference edges,1MiB aggregate logical
node ownership per domain, existing container/layout/call/work limits retained.
Tracing/reclamation occurs before allocation refusal and at safe segment points;
logical caps are not RSS guarantees and do not change containment limits.

One graph transaction resolves every alias to the same working node version.
Mutations stage validated replacement states and publish through nonthrowing
commit after all allocation/preparation. Uncaught faults/cancellation discard the
current graph and root-state segment; source catches retain prior working changes.
Constructor savepoints discard provisional identities and all constructor graph
writes on failure, while explicit shared Atomic/concurrent/synchronization effects
retain their established outside-rollback behavior. Required non-null fields are
validated before successful constructor publication.

Await boundaries commit and resumption refreshes root and graph state. Captured
references preserve identity across updates by other owner actions. Local child
coroutines share root ownership, cancellation, normal/cleanup budgets and scope.
Successful return publishes after cleanup; existing host-result authority and
primary-fault precedence remain. Source steps, allocation and dispatch consume
existing tickets; virtual dispatch cannot bypass static or dynamic work bounds.

## Completed implementation and proof

Implemented the explicit v3 source/module request, ordinary object construction,
private/public instance members and constructors, source-interface conformance,
checked upcasts and closed dispatch. Reference-bearing owner-local collections,
async receiver/argument lifetime, alias-aware root/graph publication, constructor
savepoints and bounded cycle tracing are covered by independent runtime and
source fixtures. Generated payload slots use internal names so valid fields such
as clone/validate/trace/logicalBytes/validateInitialized retain their source names.

Terminal source-built headless acceptance:
`/tmp/pipelang-objects-20261030/final-native-2/acceptance.json`;
102 contained units,19 AddressSanitizer/UndefinedBehaviorSanitizer fixture pairs,
including four new runtime/source/transaction/composition pairs. Object oracles
admit positive source/module programs and assert31 semantic,4 request,27 manifest
and3 view refusals. Root check/build/test workflows also pass through the pinned
released CLI. Focused4 separately passed22 contained units. Legacy generated
Models.hpp is byte-identical and existing owned/task/collection/atomic/concurrent/
synchronization runtime headers are unchanged from the objective baseline.

The first full job was intentionally interrupted to fix valid source-field/runtime
helper name collisions; that job and earlier failed focused proof are retained,
with process trees removed. The preceding ThreadSanitizer host probe failed with
an unsupported runtime mapping; no race-sanitizer qualification is claimed.

Final preservation/receipt inventory:
`/tmp/pipelang-objects-20261030/acceptance.json`. MesaMoto and ForgePipe authored
bytes, unrelated PipeLang bytes and all Git HEAD/branch/index anchors are
preserved. No commit, push, installation, UI startup or consumer port occurred.
The capability remains explicitly selected headless C++ support; deferred scope
and limits above and in docs/concepts/pipelang-objects.md remain in force.
