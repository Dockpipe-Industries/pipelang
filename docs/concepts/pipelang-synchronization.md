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
asynchronous finally.

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
registrations before a stale continuation can run. Scope teardown first preserves
already produced host outcomes, then cancels pending polling waits on the owner
without invoking acquisition predicates or consuming new permits. Authored finally
is attempted under its existing reserve before remaining frame/state release;
cleanup exhaustion does not promise every authored cleanup effect completes.

Both successful and exceptional resumption refresh the working transaction from
live state before source recovery/finally. Only actual suspension begins a fresh
normal/cleanup evaluation segment. Immediate acquisition, timeout-zero completion,
closing refusal and local-child completion do not replenish budgets.

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
channels, general resource disposal and asynchronous finally remain separate
capabilities. ThreadSanitizer execution is unavailable on the currently qualified
host; this is not race-sanitizer, performance, platform or release qualification.
