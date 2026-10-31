# Native model modules, overloads and closed generics

The explicitly selected C++20 ordinary-model compiler supports closed source
modules, exact method/constructor overloads, generic owned records and local
generic methods. These capabilities belong to `src/native/model-compiler`; they
do not extend the Go/Core compiler, historical view source profiles or the
experimental admitted-Core emitter. Source-owned headless compilation/runtime fixtures cover these contracts.
Qualification and its platform limits are described in
[the native acceptance guide](../../tests/native-models/README.md).

[Source objects and interfaces](pipelang-objects.md) are an explicit v3
continuation. That profile distinguishes exported model roots from ordinary
instances and separates owner-confined reference containers from owned values.

## Closed modules

Each module declares its identity before its imports and public declarations:

```pipelang
module Values;

public Record Box<T> {
    public T Value;
}
```

Its consumer imports that module explicitly:

```pipelang
module App;
import Values;

public Class Counter {
    public pure int Read() {
        Box<int> box = new Box<int> { Value = 12 };
        return box.Value;
    }
}
```

Select `pipelang.native-model.v2` and supply every source directly. This complete
request demonstrates the transport independently of source-file discovery:

```json
{
  "protocol": "pipelang.native-model.v2",
  "entry": "App",
  "namespace": "example",
  "modules": [
    {
      "name": "Values",
      "source": "module Values; public Record Box<T> { public T Value; }"
    },
    {
      "name": "App",
      "source": "module App; import Values; public Class Counter { public pure int Read() { Box<int> box = new Box<int> { Value = 12 }; return box.Value; } }"
    }
  ]
}
```

The module declaration must match the supplied `name`. All supplied modules
must belong to the entry's import closure. Missing/duplicate modules, duplicate
imports, self-imports, cycles and unreachable supplied modules refuse. The
compiler performs no filesystem/network lookup, package/version resolution or
source concatenation. Public nominal declarations form each module's exports;
imports do not introduce top-level functions or transitive re-exports.

Unqualified type names prefer the defining module, then its direct imports.
Two imported matches are ambiguous. A qualified name such as `Values.Box<int>`
still requires a direct `import Values;`. Parameter/local bindings retain their
lexical scopes, including loop items, catch bindings and finally blocks. Host
interface methods must be public; existing class member visibility still applies.

Every source diagnostic retains its original module-local byte offset. V2
diagnostics additionally identify the source module. Imported declarations lower
through compiler-owned native identities; entry classes retain their authored
names. Authored identities remain available in manifest metadata.

## Exact overloads and explicit generics

Methods may share a name when their ordered parameter types differ:

```pipelang
public Class Helpers {
    public pure int Add(int value) => value + 2;
    public pure float Add(float value) => value + 0.5;
    public pure T Identity<T>(T value) => value;

    public pure int Integer() => Identity<int>(Add(3));
    public pure float Floating() => Identity<float>(Add(1.0));
}
```

Arguments are typed once and must match exactly. `Add(3)` selects `int`;
`Add(1.0)` selects `float`. Implicit numeric/reference conversions do not
participate. Return type, parameter names and pure/async flags do not distinguish
signatures. Duplicate exact signatures refuse. Constructors use the same exact
signature distinction and retain a separately checked field-initializer tree
for each overload.

User generic declarations currently support `Record<T,...>` and local methods
on ordinary classes. Record fields must be public owned values. Generic records
declare fields only. Supply every type argument explicitly, both in nominal
types and generic calls. Explicit generic calls select the generic family;
nongeneric calls do not infer generic type arguments.

The compiler binds closed arguments, independently clones the template's AST and
defaults, and checks the resulting concrete definition before emission. Repeated
applications reuse one specialization. Parsing and structural validation also
apply to unused templates; v2 additionally resolves their module type references.
Concrete expression typing and owned-value/effect checks are deferred until
instantiated. This is a closed
specialization contract, rather than universal validation of every possible
type argument.

Generic classes/interfaces, class inheritance, constraints, inference and generic
instance-member dispatch remain unsupported. Recursion that changes an active
template's arguments refuses. Existing record-layout and method-call cycle
checks also reject recursive concrete graphs.

## Limits and consumer boundary

Limits apply to the complete request, rather than independently increasing with
each module:

| Resource | Bound |
| --- | --- |
| Source bytes | 65,536 total |
| Tokens | 32,768 total, including each source's EOF |
| Source expression nodes | 16,384 total |
| Supplied modules / declarations | 128 modules; 128 total declarations |
| Members / parameters | 256 members per declaration; 32 parameters/type parameters |
| Type nesting | 12 |
| Closed generic records / methods | 128 record specializations; 256 method specializations, within declaration/member limits |
| Specialization AST/type copies | 65,536 cumulative nodes, claimed before allocation; includes temporary type bindings |

`pipelang.native-model.v1` remains the single-source request with `protocol`,
`source` and optional `namespace`. It supports the additive overload/generic
syntax, while module/import declarations require v2. Each protocol rejects the
other transport's extra fields. Existing source without new constructs retains
its previous response/header/manifest bytes.

The model manifest retains its v1 protocol and adds optional `sourceName` only
where authored module/specialization identities are needed. Classes with multiple
constructors additionally expose `constructorOverloads`, containing every exact
`{parameters}` entry. The legacy `constructor` equals the first entry; classes
with one constructor omit the extension. Headless SDK manifest admission checks
these fields and duplicate signatures strictly.

The native view compiler and SDK view-binding helper explicitly refuse modules,
specialized generics and overloaded methods/constructors until that bridge is
independently qualified. Existing view generation keeps its current contract.
These capabilities do not imply package distribution, cross-platform release,
UI integration or new consumer deployment proof.

## Parallel iteration and explicit atomic state

Effect methods can use `parallel foreach` over `List<T>`, `Array<T>`,
`Dictionary<K,V>`, `HashSet<T>`, `Queue<T>`, `Stack<T>` and the shared concurrent
collections described below. The source expression is
evaluated once, and workers consume one owned snapshot in the collection's
iteration order. Each iteration receives its own item value. Execution and atomic
update order are concurrent; source order does not imply completion order.

```pipelang
public Class Totals {
    private Atomic<int> total;

    private void Accumulate(Atomic<int> destination, int value) {
        destination.Add(value);
    }

    public int Sum(List<int> values) {
        total.Store(0);
        parallel foreach (int item in values) {
            Accumulate(total, item);
        }
        return total.Load();
    }
}
```

Workers may read outer ordinary values and the enclosing method's working-state
snapshot, but cannot mutate them. Worker locals may change, and worker-local
loops, `break`, `continue`, `try`, `catch` and `finally` retain their usual local
scope. Returning from the enclosing method, escaping the parallel loop, nested
parallel work, host service calls and `await` are refused. Helpers, including
explicit generic specializations, are allowed when their transitive behavior is
worker safe. Calling a helper does not bypass these restrictions. Pure methods
cannot perform parallel iteration or atomic operations.

Native hosts configure an additional `pipelang::model::ParallelOptions` argument
after `TaskOptions` when constructing models that contain parallel work. The
default maximum degree is 4; admitted degrees are 1 through 32. A degree of 1
still uses joined worker execution. Every started worker is joined before control
leaves the operation, including when callback copying or thread construction
fails. Cleanup cannot start new parallel work.

An item fault does not immediately stop other items. After joining, the runtime
rethrows the recorded non-cancellation item fault with the lowest snapshot index.
Recorded faults take precedence over cancellation. Cancellation is cooperative:
workers check inherited cancellation before items and at normal evaluation
steps. Cancellation reaches synchronous invocation and asynchronous root outcomes
as `Cancelled`; source `catch (Error error)` cannot intercept it. Authored
`finally` still runs under the cleanup reserve. No item is implicitly retried.

Workers share the enclosing invocation's remaining normal evaluation tickets and
cleanup tickets. Each pool starts with at most 65,536 tickets; parallelism does
not reset or multiply either allowance. Cleanup depth is local to each worker,
while consumption of the reserve is shared. Resource exhaustion is a failure,
and source Error handling cannot turn an evaluation limit into an unbounded run.

`Atomic<T>` is an explicit shared-lifetime cell for `int`, `bool` or `float`.
Default construction creates a zero/false cell; `Atomic<T>.Create(value)` supplies
an initial value. Copying a handle shares the cell. Atomic operations use
sequential consistency and expose values rather than mutable storage references.

| Source operation | Result |
| --- | --- |
| `.Load()` | Current value |
| `.Store(value)` | No value |
| `.Exchange(value)` | Previous value |
| `.CompareExchange(value, comparand)` | Observed previous value; stores `value` only when the comparison matches |
| `.Add(delta)`, `.Increment()`, `.Decrement()` | New value; `int` cells only |

Integer updates check signed 64-bit overflow before changing the cell.
CAS retries consume the shared evaluation budget and are independently capped;
contention cannot create an unbounded retry loop. Float
compare-exchange compares binary64 bits, including signed zero and quiet NaN
payloads; atomic floating arithmetic is unsupported. Return values from mutating
atomic operations may be discarded. This exception does not permit discarding
ordinary non-void helper results.

Atomic handles may be private fields, explicit parameters or locals. They cannot
appear in public fields, ordinary records, snapshots, return types, results or
collections. Private field cell identity is fixed after construction. Atomic
effects persist when a surrounding ordinary-state transaction fails or is
cancelled; they are not rolled back with ordinary model state. Several separate
atomic updates do not form a transaction.

## Owned collection surface

All collections expose `.Count`; Array also exposes `.Length`. Ordinary and
parallel foreach use copied iteration values. Container copies preserve value
semantics even where the implementation shares immutable internal storage.

| Type | Construction and operations | Iteration |
| --- | --- | --- |
| `List<T>` | `.Empty()`, `.Add(value)`, `.At(index)` | Append order |
| `Array<T>` | `.Create(length, initial)`, `.At(index)`, `.Set(index, value)` | Index order; fixed length |
| `Dictionary<K,V>` | `.Empty()`, `.Add(key, value)`, `.Set(key, value)`, `.At(key)`, `.TryGet(key)`, `.ContainsKey(key)`, `.Remove(key)`, `.Clear()`; `.Keys`, `.Values`, `.Entries` are owned Lists | Insertion order of `KeyValuePair<K,V>` values with `.Key` and `.Value` |
| `HashSet<T>` | `.Empty()`, `.Add(value)`, `.Remove(value)`, `.Contains(value)`, `.ToList()`, `.Clear()` | Insertion order |
| `Queue<T>` | `.Empty()`, `.Enqueue(value)`, `.Dequeue()`, `.TryDequeue()`, `.Peek()`, `.Clear()` | Front to back |
| `Stack<T>` | `.Empty()`, `.Push(value)`, `.Pop()`, `.TryPop()`, `.Peek()`, `.Clear()` | Top to bottom |

`KeyValuePair<K,V>.Create(key, value)` constructs an owned pair with read-only
`.Key` and `.Value` properties.

Dictionary keys and HashSet elements are limited to `int`, `bool`, `string` or an
enum. Dictionary `Add` refuses an existing key; `Set` replaces or inserts.
`TryGet`, `TryDequeue` and `TryPop` return `Optional<T>`. Required lookups, invalid
indices and removal/peek from an empty queue or stack fail through ordinary
source Error handling. HashSet `Add` and `Remove`, and Dictionary `Remove`, return
whether membership changed.

The recursive ownership caps remain 4,096 aggregate elements and 1 MiB of logical
owned bytes per container, including nested values. These deterministic logical
costs exclude allocator overhead and are distinct from process containment.
Collection payloads must be admitted owned values; tasks, services, cancellation
tokens, Atomic handles and concurrent collection handles cannot enter nested
owned containers.

## Shared concurrent collections

`ConcurrentDictionary<K,V>`, `ConcurrentQueue<T>` and `ConcurrentStack<T>` are
explicit shared-lifetime handles. Default construction or `.Empty()` creates an
empty cell; copying a handle shares its identity. Each cell synchronizes complete
operations with its own mutex. Operations are linearizable, without lock-free,
fairness or multi-operation transaction guarantees.

```pipelang
public Class Results {
    private ConcurrentDictionary<int,int> values;

    private void Store(ConcurrentDictionary<int,int> destination, int item) {
        destination.TryAdd(item, item * 2);
    }

    public int Build(List<int> input) {
        parallel foreach (int item in input) {
            Store(values, item);
        }
        return values.Count;
    }
}
```

All three types expose `.Count` and `.Clear()`. Supported operations are:

| Type | Operations and owned snapshots |
| --- | --- |
| `ConcurrentDictionary<K,V>` | `.TryAdd(key,value)`, `.Set(key,value)`, `.At(key)`, `.TryGet(key)`, `.ContainsKey(key)`, `.TryRemove(key)`, `.GetOrAdd(key,value)`, `.TryUpdate(key,replacement,comparison)`; `.Keys`, `.Values`, `.Entries` return owned Lists |
| `ConcurrentQueue<T>` | `.Enqueue(value)`, `.TryDequeue()`, `.TryPeek()`, `.ToList()` |
| `ConcurrentStack<T>` | `.Push(value)`, `.TryPop()`, `.TryPeek()`, `.ToList()` |

`TryAdd`, `ContainsKey` and `TryUpdate` return `bool`; `GetOrAdd` returns the
existing or inserted value. Its supplied value is evaluated eagerly; factory
delegates are unsupported. `TryUpdate` compares the current owned value using
ordinary value equality before replacing it atomically. `TryGet`, `TryRemove`,
`TryDequeue`, `TryPop` and `TryPeek` return `Optional<T>`. `At` fails for a missing
key. Mutating operation results may be discarded.

Dictionary keys are `int`, `bool`, `string` or enums; payloads must recursively
contain admitted owned values. Each cell enforces 4,096 aggregate elements and
1 MiB of logical owned bytes, including nested values. These caps exclude
allocator overhead. Allocation/copy failure leaves the attempted mutation
unpublished. Snapshot creation may also fail through ordinary error handling.

Ordinary and parallel foreach take one owned ordered snapshot, then release the
cell mutex before running any loop body. Loop mutations therefore affect later
operations, rather than changing the current iteration set. Dictionary snapshots
use insertion order, queue snapshots are FIFO, and stack snapshots are LIFO.
Concurrent insertion/operation order depends on scheduling.

Handles may be explicit locals, model parameters or private fields. Model async
parameters retain their handles in owned frames. A private field's cell identity
is fixed after construction. Handles cannot enter public fields, ordinary
records/containers, snapshots, returned handles or Result/Task payloads. Host
service interfaces cannot transport them. Pure methods cannot construct,
observe, iterate or mutate their state. Parallel workers and transitively checked
synchronous helpers may operate on explicit handles; ordinary outer captures
remain read-only.

Shared changes persist if the surrounding ordinary-state transaction later
fails or is cancelled. Several calls, or calls across different cells, do not
form a transaction. Owned snapshots remain independent from later shared changes.
[Scoped Lock and SemaphoreSlim](pipelang-synchronization.md) now provide source
synchronization. Arbitrary Monitor/object locks, callback factories, channels and
backpressure remain separate capabilities.

The headless SDK installs `pipelang/model/concurrent_collections.hpp`; generated
headers include it only when needed. Existing model wire fields remain unchanged.
The optional view bridge does not admit shared-handle constructor/action
parameters or return transport. Private shared implementation state remains
outside exported owned snapshots.
