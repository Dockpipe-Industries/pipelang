# Source Lock and SemaphoreSlim

The explicitly selected C++20 ordinary-model compiler supports dedicated `Lock`
and `SemaphoreSlim` shared handles. The implementation belongs to the headless
model compiler/SDK; it does not extend the legacy Go/Core profiles or provide
complete .NET API/ABI compatibility. Source-owned acceptance fixtures are
reproduced through [the native qualification guide](../../tests/native-models/README.md).

## Scoped locks

A default `Lock` or `Lock.Create()` creates a reentrant shared cell. Copying a
handle shares that cell. A lock statement evaluates its operand once, retains
its lifetime and releases the acquisition when its lexical scope exits:

```pipelang
public Class Counter {
    private Lock gate;
    private Atomic<int> count;

    public int Increment() {
        lock (gate) {
            int current = count.Load();
            count.Store(current + 1);
            return count.Load();
        }
    }
}
```

The native guard releases on normal completion, return, break/continue, source
faults, cancellation and evaluation exhaustion. Nested acquisitions of the same
cell on the same thread are reentrant. Guard lifetime ends before the cell can
be released. Acquisition retries consume the invocation's normal evaluation
budget and check cancellation. Locks acquired during finally use the shared
cleanup reserve and ignore cancellation there; the reserve remains finite.

A lock body and its transitive helper calls cannot suspend, launch parallel work
or block on a semaphore. Pure methods cannot acquire locks. Workers may use locks
for compound operations on explicit shared handles. Ordinary outer captures and
model transaction state remain read-only in workers, including inside a lock.
A lock does not change value ownership or make model snapshots shared mutable
objects. Participants must use the same lock to coordinate a sequence of calls.
There is no fairness or global lock-order guarantee.

## Explicit semaphore permits

`SemaphoreSlim.Create(initial)` uses a maximum of 4,096 permits;
`SemaphoreSlim.Create(initial, maximum)` specifies a maximum from 1 through 4,096.
Initial count must be between zero and that maximum. A default semaphore starts
with zero permits and maximum 4,096. Copies share identity and lifetime.

| Source operation | Result |
| --- | --- |
| `.CurrentCount` | Current available permits as int |
| `.Release()` / `.Release(count)` | Previous count; count must be positive and must not overflow the maximum |
| `.Wait()` / `.Wait(token)` | No value after successful acquisition |
| `.Wait(timeoutMilliseconds)` / `.Wait(timeoutMilliseconds, token)` | Bool: acquired or timed out |
| `await .WaitAsync()` / `await .WaitAsync(token)` | No value after successful acquisition |
| `await .WaitAsync(timeoutMilliseconds[, token])` | Bool: acquired or timed out |

Timeout zero performs an immediate attempt. Minus one has no wall-clock deadline;
finite timeout values are limited to 0 through 60,000 milliseconds. Synchronous
wait retries consume evaluation tickets, so an infinite synchronous wait remains
bounded by that invocation's remaining budget. Blocking Wait is refused in async,
lock and finally bodies, including their synchronous helper call closures. Use
WaitAsync in async code. Timed bool results must be consumed; Release results may
be discarded.

Release is explicit and has no thread affinity. A successful acquisition does
not create an automatically releasing lease. Ordinary model rollback or later
cancellation does not undo permit operations. Callers must pair acquired permits
with Release, ordinarily through try/finally around the protected work:

```pipelang
public Class Worker {
    private SemaphoreSlim permits = SemaphoreSlim.Create(1, 1);
    private Atomic<int> completed;

    public async Task<Result<int,string>> Run(CancellationToken token) {
        await permits.WaitAsync(token);
        try {
            completed.Increment();
            return Result<int,string>.Success(completed.Load());
        } finally {
            permits.Release();
        }
    }
}
```

Timeout returns false without consuming a permit. Cancellation before acquisition
throws typed OperationCancelled; root tasks/actions report Cancelled and source
catch(Error) cannot intercept it. Once acquisition succeeds, a later cancellation
does not relabel that grant or return its permit automatically. Invalid counts,
timeouts and release overflow are ordinary recoverable model execution faults.
Overflow checks precede mutation.

WaitAsync is a directly awaited compiler intrinsic that unwraps to void or bool.
Its non-timed overload may be an await expression statement; timed overloads must
initialize or assign a bool. This does not admit general source Task<void> or
Task<bool> declarations, task storage/escape, await in arbitrary expressions or
asynchronous finally in legacy profiles. The modern object-enabled profile supports
awaited finally under the cleanup contract below.

## Owner execution and teardown

Async waits use bounded owner-polled readiness; they occupy neither the single
host-service worker nor a new thread per waiter. Hosts must continue calling the
model's poll() to deliver continuations. Each poll captures its registrations
before delivering host completions, then attempts each captured waiter at most
once. Registrations created by those continuations belong to the next poll.
There are at most 32 registration slots, alongside the existing bounded root task
capacity.

Release makes permits available. A queued async waiter acquires during its owner
readiness probe; Release alone does not reserve a permit for that waiter. A cancel
request before that probe may therefore win even if Release happened first.
The acquired result is authoritative once the probe decrements the count. Waiter
order and fairness are not promised.

Waiters retain their shared handles in owned frames. Frame destruction invalidates
registrations before a stale continuation can run. Scope teardown closes normal admission and cancels normal operations, preserving
already produced host outcomes. Normal pending polling waits cancel without
invoking acquisition predicates or consuming new permits. Admitted source cleanup
can continue to await under its own cleanup context. The owner drains those
continuations before stopping the host worker or destroying services and resource
lifetime roots. Cleanup exhaustion or an explicitly cancelled cleanup operation
does not promise every authored cleanup effect completes.

Both successful and exceptional resumption refresh the working transaction from
live state before source recovery/finally. Only actual suspension begins a fresh
normal/cleanup evaluation segment outside cleanup. A suspension during cleanup,
including in an awaited helper, never replenishes its shared cleanup reserve.
Immediate acquisition, timeout-zero completion, closing refusal and local-child
completion do not replenish budgets.

## Ownership and integration limits

Synchronization handles may be private fields or explicit model parameters/locals,
including owned coroutine arguments. A private field's identity is fixed after
construction. Handles cannot appear in public fields, ordinary records/containers,
owned snapshots, returned handles, Result/Task payloads or host-service transport.
Pure code cannot construct, observe or mutate synchronization state.

The optional view transport does not admit synchronization-handle parameters.
Private implementation state remains outside exported owned snapshots. Generated
headers include `pipelang/model/synchronization.hpp` only when needed. Existing
source that does not select synchronization retains its generated model artifacts.

Arbitrary object/Monitor locking, condition-variable source APIs, callback factories,
channels and general resource disposal remain separate capabilities. ThreadSanitizer execution is unavailable on the currently qualified
host; this is not race-sanitizer, performance, platform or release qualification.

## Awaited finally in the modern profile

Finally may directly await host, local, source-interface, virtual and generic
source methods, or SemaphoreSlim.WaitAsync. Locks and parallel workers still
cannot suspend. Cleanup cannot start parallel work, use blocking Wait, return
from the enclosing method, propagate its Result, or break/continue out of the
cleanup scope. Local loops and nested try/catch/finally remain supported.

The protected exit is retained across suspension. A return expression is evaluated
once; its owned value and Task result are published only after applicable cleanup
finishes. A thrown cleanup fault replaces a normal exit, including an attempted
return. When a fault is already propagating, that original fault wins. Returned
Failure/Cancelled values are ordinary values; finally does not implicitly inspect
or propagate them. Source code owns any combination of phase Results.

Cleanup shields inherited normal operation cancellation. Explicit tokens passed
to cleanup operations remain effective; CancellationToken.None permits cleanup
after ordinary Task cancellation. Nested cleanup and awaited helpers inherit the
same cleanup phase. This does not renew a revoked InvocationPermit or permit stale
publication/host effects. The native port must still honor its effective token or
supply its existing bounded deadline; source work tickets cannot terminate arbitrary
native code. Keep body and cleanup deadlines distinct when the application needs
that policy.

Each await retains the usual segment publication/refresh boundary, including an
await in finally while a primary exception is pending. Writes before that await
can therefore be published; only an uncommitted tail rolls back on a later fault.
There is no atomic rollback across asynchronous cleanup or host/resource effects.
Synchronous finally retains its existing rollback behavior.

Cleanup uses the independent 65,536-ticket reserve and the same source ancestry
limit of 32. Finally itself adds no source activation or helper coroutine. At
depth32 inline cleanup may run, while another source call is refused until an
ancestor unwinds. Repeated real suspensions cannot refill cleanup tickets.

The native TaskScope owner drains admitted cleanup before destroying its frames.
Cleanup-phase authority belongs to that entry and phase: a native await descriptor
cannot be transferred to a sibling task, another scope, or a later cleanup phase.
Checks precede readiness predicates and submission. Standalone native/noop awaiter
probes remain supported. Frames deliberately suspended by native code without a
registered wakeup retain the nonthrowing abandonment fallback; source-generated
awaits always register their leaf wakeup. Forced process termination does not
establish completed cleanup. A raw frame destructor cannot perform asynchronous
source cleanup.

This capability requires the matching compiler and runtime headers. It does not
change Task.Result's public Success/Cancelled/Failure envelope, task/polling
capacities, physical containment caps or legacy profile admission.

## Independent authored cancellation tokens

In the modern object-enabled profile, source CancellationToken parameters are
ordinary independent values. Root wrappers, local/object/interface/virtual/generic
calls and composition SourceMethods do not merge them with each other or with the
implicit task token. For example, a cancelled cleanupToken does not cancel a body
operation passed bodyToken, and body cancellation does not turn CancellationToken.None
into a cancelled cleanup token. Source IsCancellationRequested observes the authored
value; Task.cancel does not mutate that value.

Task.cancel, group and context cancellation retain their separate implicit operation
context. A concrete host or semaphore operation combines that context with the token
operands actually supplied to that operation. Tokens elsewhere in the source method's
signature do not participate. Host operation admission and effective host arguments
keep the existing operation-level combination rule. Successful authoritative host
outcomes are not relabelled after completion, and this correction adds no blanket
cancellation check to every expression/evaluation-budget step.

Locks and parallel foreach have no authored token operand in the current syntax;
they inherit the implicit task/context cancellation only. An otherwise unused modern
method token no longer silently controls them. Explicit Wait/WaitAsync token operands
remain effective. Synchronous host arguments retain their existing explicit source
value contract. Legacy profiles retain their previous parameter aggregation behavior.

Composition handles additionally have a separate native boundary token. It controls
admission independently of the source arguments; only that boundary token can cause
pre-admission cancellation. Authored cancelled tokens can therefore reach source
cleanup logic rather than preventing the method from running altogether.
