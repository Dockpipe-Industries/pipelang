# Concurrent collection continuation

Objective contract:
objective_id: pipelang-concurrent-collections-20261030
state: completed
execution_skill: dorkpipe-objective-execution
execution_authority: explicit user request to add ConcurrentDictionary, ConcurrentQueue and ConcurrentStack
authorized_objective: C++-first shared collection handles with linearizable bounded operations, owned snapshot iteration and compiler ownership/effect enforcement
done_when: semantic, refusal, concurrency, allocation-failure, sanitizer and compatibility fixtures pass; root workflows pass; supported/deferred APIs documented; unrelated bytes and Git anchors preserved
inherited_invariants: explicit native selection; original wire/header compatibility; typed failures/cancellation; existing numeric, ownership and normal/cleanup budget contracts
explicit_exclusions: lock/Monitor/SemaphoreSlim implementation (discussion only); consumer porting; MesaMoto/ForgePipe edits; DockPipe repository access; commit/push/publication; system installation; live UI; full compiler campaign restart
checkpoint_policy: automatic_within_objective
verification_policy: sequential existing containedexec job.py/run.py;1GiB/unit,800MiBproactive,zeroSwap,2GiBaggregate,8GiBnewestate; retain partial/failed proofs
handoff_policy: user_requested_only
context_pressure_policy: warn_and_continue
checkpoint_output_policy: quiet_success_bounded_failure
terminal_conditions: completed | blocked | failed_verification | cancelled

Baseline: accepted prior composition/parallel 967 authored PipeLang files at original
codex/pipelang-editor-staging HEAD33fb61ca74fe94748ecd6f66b562f7b5fd53da1a;
all prior accepted authored bytes match. MesaMoto142 and ForgePipe259 unchanged.
No staged paths. Preserve this dirty checkout and previous implementations.

## Selected semantics

Shared-lifetime handles use synchronized per-cell operations, not lock-free claims.
Default/Empty creates empty cells; copies share identity. Public fields, records,
owned container/Result/Task payloads and returned handles refuse. Explicit locals,
model parameters (including owned async frames) and private fields are allowed;
private field identity is fixed after construction. Pure code cannot construct,
observe or mutate shared collection state. Host interfaces cannot transport handles.
Mutating compound operations are linearizable; returned/snapshot values are owned.
Allocation/copy failures cannot leave partly published mutations. Ordinary state
rollback does not undo shared operations. No callback factories or multi-operation
transaction guarantee. Existing4096 aggregate elements/1MiB logical ownership caps
apply per shared cell. Key types remain int/bool/string/enum.

Dictionary: Empty, TryAdd, Set, At, TryGet, ContainsKey, TryRemove, GetOrAdd(value),
TryUpdate(key,replacement,comparison), Count, Clear, owned Keys/Values/Entries.
Queue: Empty, Enqueue, TryDequeue, TryPeek, Count, Clear, ToList.
Stack: Empty, Push, TryPop, TryPeek, Count, Clear, ToList.
TryGet/TryRemove/TryDequeue/TryPop/TryPeek return Optional values.
Foreach and parallel foreach capture one owned ordered snapshot; loop bodies hold
no cell lock. Dictionary snapshots use insertion order (concurrent insertion order
is scheduler dependent); queue FIFO and stack LIFO order follow linearization.
Workers may mutate explicit shared handles; ordinary captures remain read-only.
Source synchronization objects/async semaphore waits remain separate discussion.

## Completed focused acceptance

Implemented the selected shared collection/compiler contract. Final source-built
SDK qualification passed 74 sequential contained units, thirteen sanitizer pairs
(including two new AddressSanitizer/UndefinedBehaviorSanitizer pairs), and 29 new
source refusal cases. Root check, build and test workflows passed with installed
DockPipe 0.6.33 and the previously admitted Go toolchain. Existing generated
Models.hpp bytes remain identical; existing owned/Atomic/task runtime files retain
their accepted bytes. The source and independent runtime fixtures cover shared
identity, compound operations, exactly-once concurrent producers/consumers,
owned snapshot iteration, capacity/allocation/copy failure, async handle retention
and shared effects persisting across ordinary rollback/cancellation.

The source fixture exposed quoted keywords being parsed as syntax. Text tokens
are now excluded from keyword and binary-operator admission, with both the quoted
key positive and quoted operator refusal retained. Partial failed proof is preserved.

ThreadSanitizer was not retried: the preceding probe on this host rejected its
runtime memory mapping. No race-sanitizer or performance qualification is claimed.
Application locks/monitors and SemaphoreSlim remain discussion/planned capabilities;
this objective implements collection-internal synchronization only.

Durable acceptance: `/tmp/pipelang-concurrent-collections-20261030/acceptance.json`.
Exact source SDK proof: `/tmp/pipelang-concurrent-collections-20261030/final-native/acceptance.json`.
Preservation covers unrelated PipeLang authored bytes, MesaMoto/ForgePipe authored
bytes, original HEAD/branch/index anchors and the retained failed/partial estate.
Consumer porting remains paused. No commit, push, publication, system installation
or live UI startup was performed.
