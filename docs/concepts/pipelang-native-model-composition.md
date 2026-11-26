# Native model modules, overloads and closed generics

The explicitly selected C++20 ordinary-model compiler supports closed source
modules, exact method/constructor overloads, generic owned records and local
generic methods. V3 and project compilation also admit explicit closed generic
instance calls on concrete source classes. These capabilities belong to `src/native/model-compiler`; they
do not extend the Go/Core compiler, historical view source profiles or the
experimental admitted-Core emitter. Source-owned headless compilation/runtime fixtures cover these contracts.
Qualification and its platform limits are described in
[the native acceptance guide](../../tests/native-models/README.md).

[Source objects and interfaces](pipelang-objects.md) are an explicit v3
continuation. That profile distinguishes exported model roots from ordinary
instances and separates owner-confined reference containers from owned values.

Local declarations can use [checked local type inference](pipelang-local-inference.md)
with `var`; API types remain explicit.

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

Modern object/project return-flow checking recognizes a literal `true` condition
on `while` and `for`, including an omitted `for` condition. A loop with no break
that targets it cannot fall through to the next statement. A reachable break
restores that exit; nested loops consume their own break/continue paths and
finally retains its existing transfer rules. Unknown or nonliteral conditions
and foreach remain conservatively zero-trip. The legacy profile is unchanged.
Non-fallthrough does not guarantee a returned value: the existing condition and
statement evaluation checkpoints still bound infinite loops, including empty
bodies, and concrete awaited operations retain their own cancellation policy.

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

V3 and project callers may select a public generic method through a concrete
instance, including a parameter, local, field, constructed value or call result:

```pipelang
public Class Assertions {
    public pure bool Equal<T>(T expected, T actual, string message) => expected == actual;
}

public Class Checks {
    public pure bool Run(Assertions assertions) {
        return assertions.Equal<int>(3, 3, "matching values");
    }
}
```

The ordinary checker owns receiver typing, overload selection, access, effects,
closed dispatch and call-cycle checks. The receiver is retained as one ordinary
call expression and evaluated once at runtime. Public imported methods and
inherited nonvirtual generic methods follow the same rules as ordinary members;
private/protected access does not become public through specialization. Generic
virtual methods and generic calls through interface receivers remain unsupported.
Generic classes/interfaces, constraints and inference remain unsupported.

Member specialization uses bounded checker discovery. Every trial checks a deep
clone; discovery discards all partial semantic metadata before adding the required
closed methods. Clones and specialized bodies share the existing cumulative
65,536 AST/type-copy budget, and the existing 256 closed-method limit remains in
force. This additional accounting applies only when instance generic applications
are present. A large request can reach the AST-copy budget before the method cap.
No whole-program receiver inference or application-specific intrinsic is added.

Recursion that changes an active
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

For GCC address-sanitized C++ builds, `EvaluationBudget::step` is marked
`noinline` to avoid repeating its fast-path checks and sanitizer metadata at
emitted AST expressions. Its body, per-expression steps, limits, cancellation
order, shared tickets and cleanup reserve remain unchanged; every ASan/UBSan
check remains enabled. Normal builds retain the original body and attributes;
the normal SourceActor control object was byte-identical.

Linux qualification binds the code snapshot with source-inventory SHA256
`506b41bd235d8a7eaf07b8bf7944c783c80b7485caa7e8d6b317136468e5caa2`
to SDK manifest SHA256
`3f492e1d55a031d223b97c6b1b564ba000a653c1cd3d96a0478eac858bee2b3b`.
The receipt at `/tmp/pipelang-budget-step-qualified-20261105/result.json`
records the full 310-unit default owner suite and installed/relocated Forge3bf
consumers in normal and sanitizer builds. Other platforms, publication and
full-game consumer qualification remain separate acceptance boundaries.

`Atomic<T>` is an explicit shared-lifetime cell for `int`, `bool`, `float` or `float32`.
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
compare-exchange compares the declared binary64/binary32 bits, including signed zero and quiet NaN
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
`TryGet`, `TryDequeue` and `TryPop` return `T?`. Required lookups, invalid
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
`TryDequeue`, `TryPop` and `TryPeek` return `T?`. `At` fails for a missing
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

Nullable source spelling and typed null/value admission are defined in [nullable values](pipelang-nullable-values.md). The old generic spelling is not an accepted source alias.

## Stateless value helpers in Source-object parallel bodies

An ordinary Source object may call its own statically selected, synchronous
`pure` value helpers implicitly from `parallel foreach`. The compiler checks the
complete helper call graph: arguments, results and locals must be deeply owned
values; helpers cannot observe fields, object references, host services, shared
synchronization handles or suspension. Explicit receivers, inherited methods,
virtual/interface dispatch, constructors and generic helper methods remain
refused in this worker path. Normal owner calls retain their existing semantics.

Admitted helpers lower to uniquely qualified functions with owned value arguments
and the existing worker evaluation budget. No receiver, graph transaction or
owner state is passed or captured. The original joined parallel runtime continues
to own cancellation, bounded concurrency and lowest-input-index fault selection;
results can be folded by source index after the join. This is not concurrent
access to mutable Source objects or a relaxation of their owner confinement.

## Receiver-free native numeric exports

Project compilation adds a `native_numeric` namespace beside `source`. A public
pure numeric-value method of an eligible fieldless `model Class` has an additive
static export at `<generated namespace>::native_numeric::<module>::<class>::<method>`.
The manifest lists its source identity and signature in `nativeNumericExports`.
The authored class name aliases a compiler-reserved implementation type, so a
numeric method with the same name as its source class remains an ordinary
callable method, including overloads, in header-only and compiled projects.
Ordinary model construction, ownership checks and query behavior are unchanged.
These exports are independent numeric evaluations; callers construct no model.

`binary32Environment` is proved over the complete admitted numeric closure.
An export without `float32` channels, record fields, locals, array elements or
operations declares `"not-required"`. This includes binary64-only `float`
arithmetic and its closed typed Result/record values, and integer/boolean/enum
calculations. Record defaults and synchronous qualified/private helpers participate
in the same proof and evaluation budget. Any admitted `float32` use retains
`"caller-admitted"`, including hidden helper locals, mixed records and empty
float32 arrays. An unrelated float32 method in the same owner does not taint an
otherwise independent closure. Zero-trip counted loops still evaluate their
condition, so predicate helpers in that condition require the same complete
closure proof. A singleton inferred bound cannot hide an allocating or throwing
predicate. Unproven methods do not gain numeric exports.

This distinction removes no ordinary-model guards, changes no arithmetic or
conversion rules and inserts no new environment check. In particular, binary64
native arithmetic retains the active host rounding and denormal behavior without
an unrelated binary32 admission precondition. Arithmetic may set sticky exception
flags; `not-required` does not promise that it is an environment observation or
that arithmetic preserves those flags. The focused `native-numeric` fixture checks
metadata and both native project modes under hostile controls with independent
binary64 references, typed outcome forwarding and a first-call allocation trap.

The native facet also admits borrowed byte-text parameters. An authored
`string` parameter lowers to `std::string_view` in this facet; ordinary model
methods retain owning `std::string` parameters and existing copy semantics.
The caller keeps the view's valid storage alive throughout the synchronous call.
The view cannot escape through a returned string, a record, local string copy,
array, stored field or captured task. Pure helpers may forward it synchronously.
Native string comparisons, concatenation, literals and prefix scanning remain
unadmitted; they do not inherit a bounded-work claim merely because a view is
cheap to copy.

Two general static operations expose raw bytes without UTF decoding or NUL
termination. `string.ByteLengthIs(text, expected)` compares the full native
size to a signed64 expected length, returning false for negative expected sizes.
It never narrows an overflowing size. `string.ByteAtOr(text, index, fallback)`
returns the unsigned byte code 0..255 or the caller's signed64 fallback for any
negative or out-of-bounds index. Bounds are checked before access, including
empty/default views and extreme indices. Its range proof joins 0..255 with the
fallback interval, allowing bounded counted loops over external byte data.
Neither operation allocates or throws. Bounded pure helpers also summarize
integer returns and successful Result<int, enum> payloads under their own fresh
parameter facts. Callers still prove Success before extracting a payload. Owned
integer arrays track a conservative union of initialized and written element
ranges; copies retain their own snapshot bounds, and unknown writes widen the
union. Unknown incoming arrays/payloads retain full signed64 bounds. The shared
proof budget covers summaries and loops; no caller-local facts are smuggled into
a helper. Native signatures, manifest admission,
header and compiled callers share the same explicit borrowed-parameter rule.

`int.RoundFromFloat(value)` and `int.RoundFromFloat32(value)` round halfway
away from zero to signed64 independently of the active rounding mode. Ordinary
calls reject nonfinite or out-of-range inputs with the existing execution-failure
boundary. Native exports require a dominating finite signed64 range proof and
retain native `llround` status behavior; ordinary truncating `int.FromFloat`
and `int.FromFloat32` remain unchanged. Qualification compares `lround` on
Linux x86_64, where long and long long both have 64-bit range. This does not
establish Windows, macOS, ARM or another compiler's floating-status behavior.
The active-host analyzer additionally bounds finite binary32 multiplication:
four endpoint products receive a conservative relative-error enclosure and a
minimum-normal floor covering directed rounding, DAZ and FTZ. Proof arithmetic
stays in normal binary64, and binary32 literal bounds decode from their bits,
so compiler DAZ/FTZ cannot erase those bounds. This proof emits no extra arithmetic,
comparisons or guards into the authored program. Other arithmetic and unbounded
products still require explicit range evidence before integer conversion.

Projects may explicitly select `native_numeric_environment: "active-host"` in
the native project v1/v2 compiler request. Installed consumers select
`NATIVE_NUMERIC_ENVIRONMENT active-host` in `pipelang_add_model`; the SDK tool
uses `--native-numeric-environment active-host`. Other values and nonproject
requests refuse the selection. Omission preserves existing generated profiles,
ordinary-model guards and native numeric admission. The selected project manifest
records `nativeNumericEnvironment: "active-host"`; its float32 numeric closures
declare `binary32Environment: "active-host"`, while binary64-only closures
retain `"not-required"`. Selection participates in the build input identity,
compiled implementation plan hash and generation receipt. A plan from one
environment cannot publish artifacts for the other.

The active-host facet performs synchronous arithmetic in the calling thread's
current rounding and denormal modes. It does not admit, replace, normalize or
restore controls, and operations may raise sticky flags. Integer-to-floating
conversion through `float32.FromInt` and `float.FromInt` uses the full signed64
domain and the active target-width conversion. Arithmetic operands/results and
integer conversion inputs/results are materialized at their declared widths.
The installed target requires GNU/Clang strict floating compilation with rounding,
trapping and contraction controls; unsupported compilers and missing explicit
header contracts fail. Incompatible fast-math remains refused. Direct C++ header
consumers must preserve that same selected compilation contract.

This additive profile admits the existing closed numeric values, guarded
floating-to-int conversion, scalar arithmetic/comparisons, exact float32 widening,
and `IsFinite`, `Abs`, `CopySign`, `Floor`, `Ceiling`, `Fmod`, `ILogB` and
`ScaleB`. Both floating widths expose `ILogB(value) -> int`: finite nonzero
inputs return their unbiased binary exponent; zero and NaN return -2147483648,
and infinity returns 2147483647. The signed32 sentinels are widened to the
language's signed64 integer. Native `ilogb` status effects are retained.
`ScaleB(value, int exponent)` multiplies by a power of two at the input width;
the full signed64 exponent domain is accepted and safely clamped to native int
before scaling. Extreme exponents retain the platform's overflow/underflow,
rounding and denormal behavior. Both APIs are also ordinary-model operations.

Active `float32.FromFloat` uses a defined hardware conversion, including values
outside the finite binary32 range. It observes active SSE rounding/FTZ/DAZ and
retains conversion flags before canonicalizing signed NaNs; signalling NaNs raise
invalid. This operation is currently qualified only for Linux x86-64 SSE2 with
the strict GNU/Clang contract. Unsupported active narrowing fails compilation
when instantiated; unrelated active operations do not acquire that restriction.
The unselected narrowing overflow policy and ordinary nearest/gradual guards
remain unchanged. Other floating math intrinsics remain conservatively excluded.
Record defaults may contain fixed scalar literals/constants; computed defaults
are excluded from the active-host facet because their initialization has a separate
emission owner. Existing default-profile admission is unchanged. Arithmetic retains
its native IEEE NaN behavior; widening retains its signed canonical NaN contract.
Active floating built-in inputs are materialized to prevent literal precomputation.
No active projection changes the ordinary model's nearest/gradual admission.
The focused Linux normal and ASan/UBSan fixture compares independent native
values, flags and controls over portable rounding plus independent SSE rounding,
FTZ and DAZ combinations, and checks first-call allocation freedom, typed outcomes,
plan identity, malformed selections, conservative refusals and ordinary guards.
This is selected Linux evidence, not full-suite or other-platform qualification.

Floating comparisons materialize their operands at the declared binary32 or
binary64 width before comparing volatile reads. Ordinary and native numeric
generation share this boundary, so a comparison skipped by source short-circuit
evaluation does not execute merely because the native optimizer can speculate
its boolean result. The focused fixture checks skipped and taken `&&`/`||`
comparisons, including subnormal and signaling-NaN inputs, against independent
native comparisons and raw SSE2/AArch64 status observations where available.
Binary64 cases exercise active rounding/denormal modes; binary32 cases retain
their admitted nearest/gradual environment. This boundary does not establish
strict exception sequencing for arbitrary operand-producing arithmetic, x87
excess precision, unsupported hardware modes or incompatible fast-math options.

Admission consumes checked syntax. The native owner facet must have no fields,
inheritance, interfaces, type parameters or authored construction policy.
Inputs, locals and results may be signed 64-bit `int`, `bool`, `uint32`, `float`, `float32`, declared
enums, nongeneric fixed-size records and nullable owned values. Record fields and
nullable payloads recursively contain these same closed values.
Closed `Result<T,E>` values are admitted only at the outer boundary: `T` is a
closed value or `void`, and `E` is a declared enum or closed owned record. These outcomes retain
Success, Cancelled and Failure, typed errors and owned payloads. Ordinary owned
values and synchronous pure methods may also use enum or owned-record Result errors; model action
exception translation, host methods and async tasks retain their string-error
boundary. Construction,
forwarding, conditional selection, status queries and `Error` reads are safe.
`Value` extraction is admitted only where checked control flow proves Success.
The proof tracks immutable Result locals and parameters through status queries,
negation, short-circuit conditions, conditional expressions, branches and early
returns. Reachable branch joins retain every possible outcome. Local copies
snapshot known outcomes; scopes discard local facts. Each helper is checked with
fresh unknown parameter outcomes, so caller guards cannot justify an unchecked
helper accessor. Saved boolean aliases and relationships established only after
a Result copy remain conservatively unproven. Unchecked or incompletely guarded
access retains the ordinary model API and has no numeric export; the throwing
runtime accessor is unchanged. Nested Results and reference-bearing or string error channels are not admitted.
The existing checked record-layout bounds and transitive evaluation budget still
apply; recursive value layouts and dynamic, generic or ambiguous type aliases
cannot bypass admission.

Nullable `Value` reads require proof of Some at the exact immutable local-root
path. Nullable lifts, null, `HasValue`, negation, short-circuit conditions and
branch joins refine those facts, including nested record and Result payload
paths. Repeated helper calls do not share facts. Mutation/rebinding and unproved
reads retain the ordinary API without a numeric export. The runtime absent-value
accessor still throws. Success/Cancelled error defaults are checked recursively;
None does not construct its payload. Computed unsafe defaults remain refused.
Standalone numeric headers declare nested records in dependency order and retain
constexpr nullable construction, status and guarded payload access.
Records are owned
value snapshots. Positional and named construction, omitted-field value defaults,
local copies, conditional selection and return-by-value are admitted. Named
construction also checks and charges every field default, including defaults
subsequently overwritten by explicit initializers. Recursive defaults and
unsupported default expressions cannot enter the projection. Field
mutation, whole-record reassignment, nested records and container/reference
channels remain excluded from this projection. Returning a record does not
publish state or introduce a graph, task scope or allocation.

Closed bodies may use scalar literals/constants, record field reads, local
scalar assignments, floating arithmetic, scalar comparisons, lazy boolean and
conditional expressions, numeric intrinsics, branches and returns. Unsigned
bitwise operators and explicit wrapping operations are admitted. The exact
`float.FromFloat32(float32)` and `float32.FromFloat(float)` conversions also enter
this projection using their existing runtime IEEE conversion semantics, including
signed canonical NaNs and finite narrowing overflow boundaries. They do not add a
new cast rule. The proven signed floating-to-integer conversion below is an
additional exception. In the default profile, other integer/unsigned conversion tags, shifts and unproven
checked integer arithmetic remain excluded; range-proven indexing is described below.

`int.FromFloat` and `int.FromFloat32` enter numeric projection only where checked
control flow proves that the current input is finite and lies within
`[-2^63, 2^63)`. The upper endpoint is excluded before conversion, including the
exact positive power of two that would round from an integer maximum. The
ordinary truncating runtime conversion and its throwing failure contract remain
unchanged. Source can use `IsFinite`, ordered comparisons, negation, lazy boolean
conditions, conditional expressions, early returns and branch joins to establish
the proof. An engine-specific target range is expressed by ordinary Source guards;
there is no consumer opcode or unchecked conversion intrinsic.

Floating locals copy currently known facts. Later scalar assignment replaces
those facts, and arbitrary arithmetic does not inherit an earlier operand guard.
Copies made before a guard do not acquire facts retroactively, and saved boolean
aliases remain conservatively unproven. Each helper starts with independent
unknown scalar facts. Public signed integers and `Result<int,E>` retain the full
signed 64-bit domain, outcome/error identity and owned value semantics. Their
checked arithmetic still requires an overflow proof; accepting an integer value
does not establish an array index bound or permit unchecked payload access.

Ordered signed-integer comparisons (`<`, `<=`, `>` and `>=`) against a
singleton integer endpoint also refine the current local's interval, including
reversed comparisons and lazy branch flow. Endpoint refinement never evaluates
`MIN - 1` or `MAX + 1`; impossible branches remain unreachable without passing
malformed empty intervals to arithmetic proof. Assignment, aliases and helper
boundaries retain the same conservative fact rules described above.

Nonnegative integer division enters numeric projection when both operand
intervals are proved and the divisor's minimum is strictly positive. Its inclusive
quotient interval is `[dividend.min / divisor.max, dividend.max / divisor.min]`.
This proves absence of division by zero and signed division overflow while
preserving truncation toward zero. Unchecked, negative-divisor and negative-dividend
closures remain excluded. Multiplication before division needs its own overflow
proof; a small final quotient cannot justify an overflowing intermediate product.
The ordinary checked runtime and integer domain remain unchanged.

The focused integer fixture covers both native project modes under the existing
first-call allocation trap, with `noexcept` signatures, signed endpoints and
adjacent representable floats, NaN/infinity refusals, aliases, invalidation,
conditional/branch proofs and typed Result forwarding. The 2026-11-08 signed
comparison/division continuation additionally covers positive variable divisors,
maximal signed values, reciprocal scaling, quotient-bounded array access and
refusal after invalidated, incomplete or nonlocal guards. Its selected Linux
normal and ASan/UBSan receipts cover header and compiled native exports; they do
not establish full-suite or additional-platform qualification.

Implicit same-class,
synchronous nongeneric pure helpers are admitted only when their complete call
graph satisfies these same restrictions. Public queries are exported; private
helpers remain private in the generated facade. Recursion, unbounded loops, host effects,
suspension, synchronization, reference-bearing public channels, unproven conversions and
exception control are excluded. The fixed-extent local-array exception below
admits only finitely proven iteration. Unsupported methods retain their ordinary model API and
have no numeric export.

Local `Array<T>` values have an additional numeric projection when `T` is an
admitted scalar and `Create` has a statically known size within the ordinary
`maximumListElements` limit. Construction, owned local copies, `At`, `Set` and
snapshot `foreach` preserve ordinary value semantics using private inline native
storage. `At`/`Set` require every reaching index to be proven in range. Local
`int` literals, assignments, comparisons and overflow-proven arithmetic support
index calculations; multiplication is restricted to nonnegative ranges and
remainder requires a positive constant divisor. Public Array channels,
array rebinding, nested arrays, escaping arrays, unbounded iteration, break and
continue remain outside this projection. Ordinary dynamic arrays, their checked
accessors and their ownership/error limits remain unchanged. Each fixed visit,
array initialization/copy and flow operation consumes the shared evaluation cap.

Projects may call a public synchronous pure method through an eligible model
class name, for example `ScalarMath.Scale(value)`. This receiver-free facet
requires a nongeneric fieldless exported model with no inheritance, interfaces
or authored constructor policy. It creates no actor and shares the caller's
ordinary evaluation/execution context through the complete helper graph.
Qualified calls and their callee work count toward ordinary expanded-work limits.
Direct and transitive use from parallel workers remains refused. Numeric
projection independently proves every qualified helper with fresh parameter
facts and a shared whole-graph allowance; it does not inherit caller proofs or
admit effects, receiver state or recursive helpers.

Canonical counted `while` loops also enter numeric projection when a local
integer counter starts at a proven singleton, compares against a proven singleton
bound with `<` or `<=`, and ends each body with a positive singleton increment.
Other writes to the counter, bound or step dependencies are refused. Every visit,
condition and increment is checked and charged under the existing allowance;
overflow, excessive work, break, continue and cleanup remain excluded. Nested
loops and zero-trip inner ranges keep their ordinary ordering. Unreachable bodies
still require well-typed lowering metadata; unreachable array-return alternatives
must retain a consistent inline representation.

Private pure helpers may use fixed-extent scalar Array parameters and returns
inside a closed numeric root. Successful call-graph proof supplies each private
signature's inline extent. Each helper is rechecked under the same budget, with
fresh Result and scalar facts; array parameters are owned snapshots, so local
mutation cannot change the caller. Conflicting parameter/return shapes in one
root, dynamic shapes, recursion and public Array exports remain refused. Separate
roots can receive private overloads distinguished by their fixed Array parameter
types; return-only shapes must remain consistent. This does not add public Array
ABI channels or relax ordinary dynamic collection bounds.

Admission conservatively counts both branch arms and every helper invocation
against the existing `defaultEvaluationSteps` cap. Record channels in parameters,
results, expressions and local storage also consume this allowance, bounding
aggregate construction and value-copy work through helper DAGs. Outcome-flow
copies, refinements, joins and lexical cleanup share that same unchanged allowance.
This conservative accounting can exclude previously admitted methods close to
the cap; inspect the exact SDK manifest and requalify affected consumers.
Sharing helpers does not reset or multiply the cap. The resulting finite
projection needs no runtime evaluation-budget or task/graph ownership state.

The generated static operations are allocation-free and `noexcept`, including
the first invocation. Binary32 callers must first admit the normal strict IEEE
rounding/gradual-underflow environment on their calling thread, and keep it valid
through the call. Direct exports do not change or recheck that environment.
Source operation ordering, materialized binary32 rounding and intrinsic behavior
are shared with ordinary generation. IEEE NaN, infinity and signed zero retain
the existing scalar semantics. This is a C++ native projection, not a stable C ABI.

`float32.ToBits(float32) -> uint32` and `float32.FromBits(uint32) -> float32`
reinterpret exactly 32 bits, rather than performing numeric conversion. Generated
C++ uses `std::bit_cast`; signed zeros, subnormal encodings, infinities and NaN
payloads round-trip without arithmetic. These two operations, integer bitwise
classification and copies of their closed results do not require the floating
rounding/denormal modes used by arithmetic and do not mutate floating exception
flags or controls. The native export facade does not insert a floating-environment
guard. Ordinary model entrypoints retain their existing environment checks.
The metadata proof above conservatively retains `caller-admitted` for these
float32 channels even when a particular bit-only operation needs no such modes.
The focused `native-numeric` fixture qualifies both project export modes with an
allocation trap and hostile rounding/denormal settings; this subset does not
establish cross-platform floating ABI or full-suite acceptance.


## Current-thread floating-environment observation

`FloatingEnvironment.Observe()` is a synchronous effect returning a `uint32`
snapshot of normalized current-thread facts. It cannot be used as pure or
constant computation. Named constants identify portable and binary32 rounding
knowledge/modes (`PortableRoundingKnown`, `PortableNearest`, `PortableDown`,
`PortableUp`, `PortableTowardZero` and corresponding `Binary32` names), and input/
result underflow knowledge (`Binary32InputsKnown`, `Binary32GradualInputs`,
`Binary32ResultsKnown`, `Binary32GradualResults`). Positive facts imply knowledge;
absent knowledge is unknown rather than permission to assume a mode. Snapshots
are values, not durable admission: callers reobserve after control changes,
thread changes or a later calculation boundary.

The runtime reads `fegetround` and binary32 controls from x86/SSE or GCC/Clang
AArch64 FPCR. It does no floating arithmetic, clears no flags and writes no
controls. Native register layouts are private runtime details. Other CPU families
and unimplemented compiler profiles report only recognized portable rounding
facts; their binary32 hardware facts remain unknown. The existing ordinary
binary32 arithmetic guard retains its separate portable fallback behavior.
Each OS/compiler profile requires native qualification.

The separate `native_observation` facade and `nativeObservationExports` manifest
profile preserve observation as an effect. Entries declare
`effects: "current-thread-observation"`, `binary32Environment: "not-required"`,
`allocationFree: true` and `noexcept: true`. Only proven receiver-free synchronous
effect methods of eligible fieldless models are exported. Their entire closure
uses closed bool/int/uint32/enum/flat-record values, bounded local calculations,
proven helpers and the read-only environment intrinsic. Floating channels and
arithmetic, arrays, model state, arbitrary user effects, async/parallel work and
unproven fallible operations are excluded. Numeric exports retain their existing
allocation/noexcept guarantees; the closure proof above distinguishes their
binary32 environment requirement from this effectful observation profile.

The same proof lets ordinary observer methods run under rejected modes despite
unrelated float32 declarations in their project. It does not remove arithmetic
method guards or broaden constructor semantics. Focused normal/sanitizer native and ordinary observer fixture qualification
passed, including retained arithmetic guard refusal. Full suite, other platforms
and releases are separate.


### AArch64 environment observations

The GCC/Clang AArch64 runtime reads FPCR directly, once per observation, without
floating arithmetic or control/status writes. It decodes RMode, FZ and FIZ into
the same public knowledge/mode facts as the SSE observer. AH alternate handling
can override rounding and underflow for some instructions, so AH-enabled
snapshots report binary32 hardware facts as unknown. Portable rounding facts
remain available. Source admission consequently refuses that alternate profile.
This is a deliberate conservative contract, not a promise that every Arm
instruction obeys the ordinary rounding/underflow controls.

The independent decoder corpus runs on every host. Native ARM64 fixtures preserve
FPCR/FPSR exactly, verify requested control readback, and exercise AH/FIZ only
with positive OS AFP capability. The verified `m6` job additionally requires AFP;
missing capability cannot silently reduce its claimed coverage. Ordinary
arithmetic guard tests retain their existing probe semantics, independently of
the stronger Source admission policy. Other CPU families and MSVC ARM64 remain
unimplemented observation profiles. Native Darwin ARM64 qualification passed on
the AFP-capable `m6` worker through DockPipe: the installed SDK baseline and focused normal
and ASan/UBSan native and ordinary-model fixtures passed. Each observer fixture
exercised 128 native control combinations and 160 synthetic decoder cases. Linux
x86/SSE normal/sanitizer regression also passed. The decoder corpus does not
qualify Linux ARM64, MSVC ARM64 or other OS/compiler combinations.

See [Arm's FPCR definition](https://support.arm.com/documentation/102807/0001/AArch64-System-registers/AArch64-Generic-System-Control-registers-summary/FPCR--Floating-point-Control-Register)
and [Apple's capability test](https://raw.githubusercontent.com/apple-oss-distributions/xnu/main/tests/arm_cpu_capabilities.c).


## Standalone caller-host numeric headers

Project compilation additionally writes `Numeric.hpp` in header and compiled
projects. Including that header explicitly selects the caller-host projection at
`<generated>::numeric_header::native_numeric::<module>::<class>`. Its separate
closed record and enum values use `<generated>::numeric_header::source` aliases;
they are transport values, distinct from the ordinary model's C++ types.

The same checked closure proof owns admission, integer overflow bounds, typed
Result access and finite loop limits. Standalone admission additionally requires
fixed scalar record defaults, excludes binary64-to-binary32 narrowing and
ILogB/ScaleB, and never emits ownership, tasks, interfaces or ordinary model
headers. Methods needing those capabilities remain available through their
existing model contracts. Only closures containing integer, unsigned, boolean,
enum, closed integer/nullable record and typed Result operations acquire generated C++20
`constexpr` methods and cross-owner calls. This generated qualifier is the
inspectable constant-evaluation contract. Result construction, outcome/error
access and successful payload access preserve their existing runtime failures.

Floating methods preserve the caller's native C++ compilation policy: natural
arithmetic and comparisons, safe integer-to-floating and widening casts, guarded
floating-to-integer casts, and standard math calls. Literal folding, contraction,
rounding and floating status therefore follow the caller's compiler options.
The header adds no volatile execution barrier, changes no floating controls, and
defines no active-host macro or compiler option. It does not promise the explicit
execution/status behavior of the existing active-host `Model.hpp` projection.
Ordinary and active-host model emission and their admission contracts are
unchanged. Both headers can coexist in either include order and separate
translation units. Public SDKs may install the generated header with its exact
language runtime include closure without exposing the model/task runtime.


The caller-host emitter uses direct scalar expressions only when at most one
operand can have evaluation effects. Literal values, local reads and closed record
field reads are neutral; calls and arithmetic retain conservative ordering.
Complete named record construction becomes a native aggregate initializer only
when authored fields follow declaration order, preserving left-to-right field
evaluation. Other argument and field orders keep sequenced lowering. This avoids
artificial lambda captures, moved scalar temporaries and record assignments that
can inhibit native literal folding under sanitizer instrumentation. Ordinary and
active-host projection lowering retains its existing sequencing/materialization.

Floating `float.Clamp(value, minimum, maximum)` and
`float32.Clamp(value, minimum, maximum)` require finite arguments and
ordered bounds; invalid arguments raise the ordinary numeric execution failure.
Native numeric closures admit these calls only after proving those preconditions.
The caller-host numeric header emits `std::clamp` in the caller's expression
context, retaining native literal folding and argument lifetime behavior. Existing
integer rounding operations retain their `llround` contract.


The active-host projection also admits binary32 and binary64 `Sqrt`. Existing
width materialization and standard math lowering preserve active rounding,
denormal handling and floating observations; no engine-specific intrinsic is
introduced. Standalone caller-host admission is unchanged. Focused Linux x86_64
GNU 11.4 normal and ASan/UBSan qualification covers nested structured Result
errors/nullable values in header and compiled consumers, standalone constexpr
headers, conservative omission cases and active-host square-root edge values
across 64 floating profiles. This is focused language evidence, not full-suite
or other-platform acceptance.
