# Reusable composition, parallel foreach and standard data types

Objective contract:
objective_id: pipelang-composition-parallel-20261030
state: completed
execution_skill: dorkpipe-objective-execution
execution_authority: explicit owner instruction to implement reusable composition and parallel foreach with atomic variables and .NET-like data types
authorized_objective: coherent C++-first modules/imports, closed generic records/methods, exact overload resolution, bounded structured parallel foreach, explicit shared atomics and common owned collections
done_when: source/native semantic, refusal, ownership, concurrency, resource and compatibility proofs pass; exact supported and deferred contracts documented; consumer porting remains paused
inherited_invariants: explicit native selection; legacy/default wire/header compatibility; typed failures/cancellation; owned snapshots; frozen int64/binary64/IEEE behavior; unrelated bytes and Git anchors preserved
explicit_exclusions: consumer porting; MesaMoto/ForgePipe edits; DockPipe repository access; new commit/push/publication; system installation; credentials/cost; native UI startup; full compiler campaign restart
checkpoint_policy: automatic_within_objective
verification_policy: sequential existing containedexec job.py/run.py; 1GiB/unit,800MiBproactive,zeroSwap,2GiBaggregate,8GiBnewestate; retain partial/failed proof
handoff_policy: user_requested_only
context_pressure_policy: warn_and_continue
checkpoint_output_policy: quiet_success_bounded_failure
terminal_conditions: completed | blocked | failed_verification | cancelled

Baseline: original PipeLang checkout remains codex/pipelang-editor-staging at
33fb61ca74fe94748ecd6f66b562f7b5fd53da1a, no staged paths, 59 unstaged and 238
untracked files. Its 947 authored bytes match the completed cleanup acceptance.
The previous source was committed as aee0749 and merged to dev as 01a7e60;
publication does not authorize new commits for this continuation.

## Selected contract

- Exact typed overloads; no return-type-only overload or implicit conversion.
  Distinct callable identities drive recursion/work accounting and diagnostics.
- Generic records/methods specialize closed source types under existing type,
  declaration, layout and call/work bounds. Canonical authored identity remains
  inspectable beside trusted generated C++ identities. Unsupported operations fail
  during closed-instantiation checking; no fallback to Go or consumer opcode.
- Multi-source modules use an explicit v2 request with a closed name/source set,
  entry and import graph. No filesystem/network imports. Missing, cyclic or
  ambiguous imports fail. Legacy v1 remains strict and byte-compatible. Native
  view binding refuses unqualified generic/module/overload adapters clearly.
- Owned Array, Dictionary, HashSet, Queue, Stack and KeyValuePair complement List,
  Optional, Result and Error. Values copy across snapshots; mutation is checked
  and transactional. Recursive element/byte ceilings apply. Dictionaries/sets use
  actual hash lookup with deterministic snapshot order; keys initially int64,
  bool, string or enum. No claim of complete .NET API/source compatibility.
- `parallel foreach` owns its input snapshot and joins all workers before exit.
  Degree defaults to4, hardmax32 and is host/runtime configurable. Nested parallel,
  suspension, host-service access, nonlocal loop transfers and mutation of ordinary
  captures/model state are refused. Worker-local owned mutation is allowed.
  Shared mutations require explicit Atomic<int/bool/float> handles. Read-only
  snapshots and independently checked pure helpers may be captured safely.
- Atomic operations are sequentially consistent; int64 arithmetic is checked;
  exchange/CAS preserve observed values and IEEE bit representations. Shared
  atomic effects are deliberately outside model transaction rollback. Atomic
  handles cannot enter ordinary records/collections/results/snapshots, escape
  through returns or be used by pure methods. Private field identity is fixed
  after construction; explicit local/parameter sharing is lifetime-owned.
- Normal and cleanup tickets are shared across all parallel iterations/helpers;
  no per-worker reset. Cancellation is inherited/cooperative, cleanup retains its
  bounded reserve, all threads join on faults/cancel/submission failure. Faults
  are selected by input index after joining; ordinary cancellation becomes typed
  Cancelled, not a catchable Error. No detached work or implicit replay/retry.

Consumer porting remains paused throughout this objective. Source disposal,
asynchronous finally, arbitrary references, inheritance and complete standard
library/platform qualification are not implied by this contract.

## Completed focused acceptance

Implemented the selected native compiler/runtime contract and canonical
[composition/parallel API](../../concepts/pipelang-native-model-composition.md).
Final source-built SDK qualification passed 65 sequential contained units,
including eleven sanitizer fixture pairs (six combine AddressSanitizer and
UndefinedBehaviorSanitizer). Root check, build and test workflows passed using
installed DockPipe 0.6.33 and the existing released Go toolchain. Existing model
header bytes, ExecutionFailure base and Transaction definition remain identical.
A final whitespace-only layout correction expanded four Empty factory bodies;
the receipt records unchanged non-whitespace content against the qualified header.

ThreadSanitizer compiled but could not start: its runtime rejected the environment
memory mapping. No race-sanitizer qualification is claimed. Independent overlap,
atomic totals, cancellation/fault ordering, shared budgets, submission failure
and joined teardown checks passed. Consumer porting remains paused.

Durable acceptance: `/tmp/pipelang-composition-parallel-20261030/acceptance.json`;
source SDK proof: `/tmp/pipelang-composition-parallel-20261030/final-native/acceptance.json`.
The full partial/failed proof estate is retained. MesaMoto and ForgePipe authored
bytes, all original HEAD/branch/index anchors and unrelated PipeLang files are
preserved. No new commit, push, publication, system installation or live UI startup
was performed for this objective. Broader .NET API, new composition view adapters,
platform/release and full compiler qualification remain deferred.
