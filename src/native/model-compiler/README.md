# Explicit native model compiler

`pipelang-modelc` is an additive C++20 ordinary-model capability. It does not
select, replace or modify the legacy Go/Core/profile compiler paths. Build this
CMake target explicitly; consumers admit the executable's exact bytes before
invoking it. The compiler uses the existing native lexer and pinned nlohmann JSON
header. It emits typed C++ and a manifest, not a private application opcode list.

The opt-in [native project contract](../../../docs/concepts/pipelang-projects.md)
discovers conventional source roots and resolves normal module imports without
authored source/export JSON inventories. `public model Class` declares an existing
transactional root; ordinary public types retain object/value semantics and receive
stable module-qualified native names. The SDK exposes PROJECT_ROOT/SOURCE_ROOT/ENTRY
and retains old protocols for explicit compatibility fixtures.

Project source may declare `public composition Startup { ... }` with explicit
singleton/scoped/transient registrations, inputs, includes and exports. Fallible
source acquisition uses `scoped IService = factory ResourceFactory.Create;` with
an explicitly registered instance receiver. `cleanup IResourceCleanup.Close;`
selects an exact source interface method taking one `CancellationToken` and returning
`Result<void,string>`. `CancellationToken.None` supplies an empty token expression
for source partial-acquisition finally blocks. Selected factory/cleanup metadata
uses graph v2; old graphs preserve v1. The optional `CompositionFactory.hpp` facade
also admits scoped `async Task<Result<T,string>>` service methods. See the canonical
[dependency composition contract](../../../docs/concepts/dependency-injection.md)
for checked factory effects, resource adoption/unwind, task draining and limitations.

Single-source input is exactly one JSON object on stdin. Closed multi-source
modules use the explicitly selected v2 request described in the
[composition and parallel contract](../../../docs/concepts/pipelang-native-model-composition.md),
which also defines exact overloads, closed generics, parallel foreach, Atomic and
the owned and shared concurrent collection surfaces:

```json
{"protocol":"pipelang.native-model.v1","source":"public Class Counter { public int count = 0; public void Increment() { count = count + 1; } }","namespace":"counter_models"}
```

`namespace` is optional and defaults to `pipelang_models`. Unknown or duplicate
keys, invalid transport types and malformed JSON fail with exit 2, bounded stderr
and no stdout. A semantic refusal returns exit 0 with `ok:false` and
`diagnostics:[{code,message,offset}]`, preserving authored byte offsets. Success
returns exactly `{protocol,ok:true,header,manifest}`. The manifest protocol is
`pipelang.native-model-manifest.v1`; it contains namespace, records, enums,
interfaces and classes. Class entries carry public fields, public method
signatures and constructor parameters. Generic type names are canonical strings
such as `Task<Result<SettingsValues,string>>`. Private implementation and service
fields are absent from the public manifest.

The [source-object profile](../../../docs/concepts/pipelang-objects.md) explicitly
selects v3 requests with exported model roots, ordinary new C(args) instances,
source Interface implementations and owner-confined reference identity. It adds
alias-aware graph transactions, constructor savepoints and inherited-scope async
calls. Its manifest v2 separates object declarations from owned root snapshots;
view transport refuses that schema until separately qualified.

The v1/v2 grammar supports ordinary public records, enums, injected host interfaces
and stateful classes. Values include signed64 integers, IEEE binary64 `float`, explicit IEEE binary32 `float32`,
bool, owned strings, nested
records, enums and recursively owned `Result<T,string>` (including void and
record payloads). Results preserve Success/Cancelled/Failure and expose checked
Value, Error and outcome predicates. Default Result is Cancelled with empty error.
Owned `Optional<T>` defaults to None, constructs with `Some(value)`/`None()`,
exposes `HasValue` and checked `Value`, and composes recursively with records,
Results and bounded `List<T>`. Services, tasks, tokens and void cannot be Optional
or List payloads. Closed generic records and local methods are described in the
[composition and parallel contract](../../../docs/concepts/pipelang-native-model-composition.md).

Records have primitive/value-expression defaults, positional construction and
named `new Record { field = value }` construction; omitted named fields retain
defaults. Class fields have defaults, an explicit constructor or a generated
default constructor. Host interfaces are private shared-lifetime constructor
injections. Class fields are externally read through owned `snapshot()` copies;
only typed methods mutate live state. Default enum is its first declared value.

Lexical blocks, single-statement conditionals, typed locals, assignments, returns, pure
methods and ordinary synchronous method calls are admitted. Public and private
mutating helpers execute within the outer transaction. Pure methods cannot write
model state, call mutating helpers or invoke host effects. Record equality and
checked arithmetic compose normally. Typed `condition ? consequent : alternative`
expressions require a bool condition and identical owned arm types. They evaluate
the condition once and only the selected arm, with right association and lower
precedence than boolean operators. Arguments and non-short-circuit operands
are evaluated left-to-right; bool operators short-circuit. Integer arithmetic is
signed64 and checked, including minimum/-1 division and negation. Any synchronous
runtime failure rolls back the entire outer action, including nested helpers.
An explicitly returned Failure/Cancelled still commits intentional authored state
changes. Bare non-void calls must have their return consumed explicitly.

Binary64 literals support decimal and exponent forms, are rounded once by the
native frontend and emitted as exact bits. Overflow/underflow outside finite
literal admission is refused. Unary minus, arithmetic, comparisons and owned
transport preserve IEEE values, signed zero, subnormals, infinities and quiet
NaNs; implicit numeric conversions remain refused. `float.IsFinite`, `Abs`,
`Sqrt` and `Log` are pure numeric operations; domain errors follow the host IEEE
math domain, not integer checked-arithmetic rules. Strict default floating modes
are required; fast/finite-only compilation is refused. Signaling NaNs, nondefault
rounding/flush modes and cross-platform math-library bit equality are unqualified.

Signed64 `int` arithmetic includes checked `+`, `-`, `*`, `/` and `%`. Integer
`%` shares multiplication/division precedence and is left-associative. It is the
remainder of division with the quotient truncated toward zero: a nonzero result
has the dividend's sign. A zero divisor raises `integer remainder by zero` through
the ordinary failure/transaction boundary. `MIN % -1` is zero; `MIN / -1` still
fails because its quotient overflows. `%` requires two `int` operands; floating
remainder and numeric conversions are not implicit.

`uint32` is an owned unsigned 32-bit primitive, emitted as `std::uint32_t`, with default
zero. Decimal literals require lowercase `u` (`4294967295u`); hexadecimal literals
use `0x`/`0X` and lowercase `u` (`0xffffffffu`). Values outside `0..4294967295`
refuse admission. There are no implicit conversions to or from other primitives.
Ordinary `+`, `-`, `*` check overflow/underflow, and `/`, `%` check zero divisors.
Explicit `uint32.WrappingAdd`, `WrappingSubtract` and `WrappingMultiply` instead
return the low 32 bits. Unary minus is not admitted for unsigned values.

`&`, `|`, `^` and `~` require unsigned 32-bit operands and operate on exactly 32 bits.
`uint32 << int` discards high bits; `uint32 >> int` is logical. Shift counts must
be `0..31`; other counts fail through the ordinary transaction/rollback boundary.
Counts are explicit signed indices, not a numeric coercion. Precedence follows
multiplication, addition, shifts, comparisons, equality, `&`, `^`, `|`, `&&`, `||`
from tightest to loosest; binary operators associate left. Shift recognition is
expression-only and does not consume the closing angles of nested generic types.

Explicit conversions are `uint32.FromInt/FromFloat/FromFloat32` and
`int/float/float32.FromUInt32`. Signed-to-unsigned conversion checks the original
integer range. Floating-to-unsigned conversion requires a finite value, truncates
toward zero and then checks `0..4294967295`: `-0.5` becomes zero, while `-1.0`,
`2^32`, NaN and infinity fail. Unsigned-to-int/binary64 widening is exact;
unsigned-to-binary32 rounds to its IEEE precision under the existing float32
thread environment guard. These checked failures preserve ordinary rollback.

Unsigned values participate in records, model fields, snapshots, results,
closed generics and owned collections, including stable dictionary/set keys.
`Atomic<uint32>` is explicitly refused in this capability; no atomic arithmetic
contract is inferred. Exposed unsigned values are refused by typed view bridges.
Integer-only unsigned models do not include strict floating headers or require
floating thread modes; only actual floating conversions introduce that dependency.

Binary32 is the explicit ordinary-model numeric primitive

 `float32`, emitted as
native C++ `float`; existing `float` remains binary64. Decimal/exponent literals
with lowercase `f` suffix (including `1f`) round directly to binary32, with
finite range and underflow admission checked before exact bit-pattern emission.
Arithmetic, negation, comparisons and `float32.IsFinite/Abs/Sqrt/Log` preserve
the scalar type. No implicit conversions between int, float and float32 exist.
Owned records, snapshots, containers, closed generic values and Atomic cells carry
float32 normally. Unsupported view bridges reject float32 explicitly.
Each binary32 arithmetic operation materializes its result before the next source
operation, preventing contraction from changing rounding.
Binary32 evaluation requires nominal-width strict IEEE arithmetic, nearest rounding
and gradual subnormals; generated entrypoints refuse other thread modes. Runtime
arithmetic retains signed zeros and IEEE nonfinite results. Native platform math
library bit equality across platforms remains unqualified.

Both floating types also provide same-type `Sin`, `Cos`, `Tan`, `Asin`, `Acos`,
`Atan`, `Atan2`, `Exp`, `Expm1`, `Pow`, `Hypot`, `Fmod`, `Remainder`, `CopySign`,
`Cbrt`, `Tanh`, `Floor` and `Ceiling`. `Hypot` accepts two or three same-type
arguments. Their results follow the native IEEE math
library, including domain NaNs, infinities and signed zeros. `Fmod` uses a
truncated quotient; IEEE `Remainder` uses a nearest/even quotient. There is no
implicit conversion of either argument. Binary32 calls materialize a binary32
result before further source arithmetic. Cross-platform libm bit identity,
NaN payload preservation and signaling NaNs are not promised.

Explicit numeric conversions are `float.FromInt`, `float32.FromInt`,
`int.FromFloat`, `int.FromFloat32`, `float.FromFloat32` and `float32.FromFloat`.
Integer-to-floating conversion rounds to the target IEEE precision; widening
binary32 to binary64 is exact. Narrowing uses nearest/even rounding, including
gradual subnormals, signed underflow-to-zero and signed overflow-to-infinity.
Finite values above binary32 maximum are handled without an out-of-range C++
cast: values below `2^128 - 2^103` round to maximum finite, and that exact
midpoint or greater rounds to infinity. Cross-float NaNs become quiet canonical
NaNs with their sign retained; payloads are not an identity contract.
Floating-to-integer conversion truncates toward zero after refusing nonfinite
values and values outside `[-2^63, 2^63)`. Failures use the ordinary rollback
boundary; IEEE floating domain results remain successful values.

The legacy lexer entrypoint retains its grammar;
 only modelc explicitly selects
the additive Binary64 numeric syntax.

`List<T>.Empty()` creates an owned list; `Add(value)` mutates an admitted local or
model field, `Count` returns int64 and `At(index)` returns a checked owned copy.
`foreach (T item in values)` evaluates and owns the input snapshot once, then
binds an owned item copy per iteration. Editing the original list or item does
not alter that snapshot. `while`, nested loops, break and continue are admitted.
Loops never prove a method returns, because they may execute zero times. Lists
cap recursive aggregate elements at 4096 and conservative logical owned footprint
at 1 MiB, including nested wrappers/records. This is not an allocator/RSS guarantee.
Class invocations using loops, local async composition or failure control share 65,536 expression/
statement steps across synchronous helpers and child coroutines. Budget failure
uses the normal rollback/typed-failure boundary. Pure reads report ExecutionFailure.
Actual host suspension begins a fresh segment; immediate closing/refused awaits
and local-await completion do not reset the budget.

`FixedArray<T,N>` is an owned inline value whose literal extent `N` is part of
its type, from 0 through 4096. Elements must be deeply owned values. `Create(fill)`
evaluates the fill once and copies it into each element without first running
element defaults. `At(index)` returns a checked owned copy; `Set(index, value)`
checks the index and ownership limits before mutation. `Count` and `Length`
return the fixed extent. Default construction validates each actual element and
accumulates ownership costs in element order, stopping on the first failure.
These checks also run during constant evaluation. Empty arrays construct no
element but still require a valid, nonrecursive element type. Copies and
`foreach` retain owned snapshot semantics. Nested fixed storage and logical
element counts remain bounded by 4096, with the existing 1 MiB ownership limit
and record layout/depth limits unchanged.

Conventional `for` loops have one optional initializer, a boolean condition and
one optional synchronous iteration statement:

```pipelang
FixedArray<int,4> values = FixedArray<int,4>.Create(0);
for (int index = 0; index < values.Length; index = index + 1)
    values.Set(index, index);
```

The initializer may declare a local, assign a value or call a discardable method;
the iteration clause may assign or call a discardable method. An omitted
condition means true. Initializer locals are available to the condition, body
and iteration clause, then leave scope. Body locals are unavailable in the
iteration clause. `continue` runs body cleanup before the iteration statement;
`break`, return and faults skip that statement. Async initialization and bodies
retain their existing rules; the iteration clause cannot await. Steps use
explicit assignments; `++` is not implemented. Ordinary loops use the existing
evaluation budget, including loops whose termination is unknown.

The additive native numeric projection accepts closed fixed arrays in public
parameters, records, optional values and outer Result payloads when its complete
closure proves bounds, ownership costs and safe access. Dynamic `Array<T>`
remains limited to the existing proved local/private channels. Native `for`
eligibility currently requires a finite ascending integer counter; unproved
loops, counter interference and loop-local break/continue retain ordinary
execution. Optional/Result extraction still requires its corresponding outcome
proof. The active-host floating profile admits `Sin` and `Cos` for both widths;
the separate caller-host header retains its narrower operation set. These
contracts concern the selected native projection, rather than a universal
allocation-free guarantee for ordinary model methods.

[Source synchronization](../../../docs/concepts/pipelang-synchronization.md) adds
reentrant `Lock` handles and scoped `lock`, plus explicit `SemaphoreSlim` permits.
Its directly awaited WaitAsync intrinsic unwraps to void or bool; this does not
admit general source Task<void>/Task<bool> declarations or escaping tasks. Blocking
waits refuse async, lock and cleanup call closures. Shared handles retain their
private/owned-value/host-transport restrictions.

Async methods use `async Task<Result<T,string>>` and directly await typed host
service calls in local initializers or assignments. They can call ordinary
synchronous helpers and perform multiple sequential host awaits. The compiler
emits C++ coroutines and immutable owned service arguments. Transactions commit at
await boundaries and successful source returns; resumption refreshes live state
before applying further source logic. Pre-await state is deliberately visible
while work is pending. Local async helpers can also be directly awaited in local initializers or
assignments. Child frames belong to the same root scope/capacity entry, inherit
cancellation (including explicit token arguments), and retain owned arguments.
The parent commits before entering a child and refreshes after the child result;
the child uses the same host/return transaction boundaries. Child runtime faults
become typed Failure; returned Failure/Cancelled commit intentional changes.
Scope teardown joins host work and drains child/parent continuations before
releasing frames, model state or services. Already committed host success remains
authoritative. Owned values and frames have deterministic native RAII cleanup;
there are no source-owned external-resource handles in this capability.
Source failure handling uses owned `Error` values, `throw`/rethrow and
`try`/`catch (Error error)`/`finally`, as described below. Source resource disposal,
class inheritance, unmanaged references and implicit numeric conversions remain refused.
V3 source object/interface references and checked interface upcasts follow their
explicit owner/lifetime contract.
Exact parameter-type method/constructor overloads are supported. Parallel local task creation, Task escape and await inside general
expressions remain unsupported. There is no arbitrary exception hierarchy or
asynchronous finally/disposal capability.

## Structured failure and cleanup

`Error(message)` owns a validated string and exposes read-only `Message`. It can
be stored, returned and nested in ordinary owned values. `throw Error(...)` raises
a model execution fault; `throw;` rethrows the nearest lexically enclosing caught
Error. A catch binding is read-only. Checked arithmetic and checked Optional/Result
access also raise model execution faults. Source catches handle that family,
not arbitrary native exceptions or evaluation-budget exhaustion. Per-value
string/list capacity failures remain recoverable; catching them never replenishes
a budget. Host and child Failure/Cancelled Results remain typed values, not throws.

Try requires a catch and/or finally. Catch recovery keeps the working changes
made before the fault; it does not create an implicit transaction savepoint.
Uncaught faults still roll back the current segment. A returned Failure/Cancelled
intentionally commits authored changes, including successful cleanup. Async try
and catch bodies may await. The generated catch owns its Error outside the native
C++ exception handler, so suspension retains no exception-reference lifetime.

Finally sees enclosing locals/current transaction state, not try/catch locals.
It runs once on normal completion, return, loop exit, fault and owner teardown;
suspension alone does not run it. It may use synchronous helpers and fallible
expressions. It cannot return, await or transfer outside its scope; loops inside
finally may use their own break/continue. Return payloads own their evaluated
values before cleanup. Normal cleanup failure replaces a pending return; a
secondary cleanup failure during unwinding preserves the primary fault. Uncaught
failure rolls back cleanup writes as part of the current segment. Async returns
commit only after cleanup succeeds, and task results publish only at final
suspension after cleanup. Recovery from a failed return may therefore await without
publishing the abandoned return value. Forced frame destruction suppresses cleanup
faults and cannot commit an earlier interrupted return.

One shared 65,536-step cleanup reserve per invocation/async segment is separate
from normal execution. Nested guards and their helpers share that reserve. No
catch, guard or local await replenishes it; only actual host suspension starts a
fresh segment. Cleanup is attempted under the reserve, not guaranteed to complete
all authored effects after a fault or exhaustion. Native owned values/frames still
release deterministically. These semantics qualify source control flow, not
external-resource handles, disposal protocols or asynchronous cleanup.

Metered synchronous assignments evaluate and bind the checked target before the
right-hand expression. Assignment performs no additional source evaluation or
cancellation step after an authoritative host result returns and before storing
that result. This lets a protected local or owned-record slot expose an acquired
capability to its finally block even when the host cancels the request as it returns.
Ordinary source-object field assignment already snapshots its receiver before the
right-hand side. Indexed/call-result lvalues are not admitted; collection update
methods retain their ordinary argument and validation semantics.

Awaited assignment remains a separate existing boundary: its target is evaluated
when the awaited value is stored after resumption. The compiler does not retain a
raw target reference across a potentially refreshed transaction. This checkpoint
does not add pre-suspension target binding or asynchronous factory acquisition.

Admission uses an explicit [compiler resource profile](../../../docs/concepts/compiler-resource-budgets.md)
for input/indexed bytes, transport, tokens, expressions, declarations, module
identities and implementation units. There is no independent 64 KiB ordinary
source ceiling. Existing language-profile checks still include 256 members, 32 parameters, bounded
syntax/type nesting (including depth48 statements), depth32 acyclic local call
graphs, at most 65,536 statically expanded source evaluation operations per method, and depth32/256 expanded nodes per
record or model value layout. Loop bodies count once in static admission; dynamic
iteration is independently budgeted as above. Work counting includes repeated callsites and both
branches, and applies to effects as well as pure getters. It does not depend on
declaration order. Strings require valid Unicode scalar UTF-8 and have a runtime 1 MiB per-value ceiling. Runtime task
capacity is independently bounded as described in the SDK runtime README.

Focused fixtures live in `tests/native-models`. Compiler execution, generated
compilation and native test execution must use the existing `containedexec`
`job.py` and `run.py`; these files are not permission to run outside containment.


## UTF-8 byte text operations

`ByteBuffer` is a separate owned binary value with a default empty value and a
1 MiB payload limit. `Empty()` and checked `FromUtf8(string)` construct buffers.
`Length`, checked `At(int)` returning `uint32`, binary-safe `Slice(start,count)`
and ordinal `IndexOf(ByteBuffer,start)` operate on byte positions. Empty patterns
match the start, including the end. Negative/out-of-range positions or counts fail.
`Add(uint32)` requires 0..255; `Append(ByteBuffer)` appends an owned copy, including
self-append. Mutations follow existing assignment, purity and transaction rules.
`DecodeUtf8()` explicitly validates text and fails for invalid or incomplete UTF-8;
binary slicing and transport never decode implicitly. Buffers compose with records,
Results, model snapshots and synchronous/asynchronous owned host payloads. Payload
bytes count toward recursive ownership bytes, without charging a collection element
per byte. Nested collections retain their existing aggregate budgets. Native code
uses `pipelang::model::ByteBuffer`, an explicit owning `vector<uint8_t>` constructor
and read-only `values()`; no borrowed native pointers enter source values.

`string.Utf8Length(value)` returns the signed integer UTF-8 byte length.
`string.Utf8Slice(value, start, count)` returns an owned byte slice. Negative or
out-of-range bounds, including an overflow-sized count, fail through the existing
execution/transaction boundary. Both slice endpoints must lie between complete
Unicode scalars; empty slices are valid at scalar boundaries, including the end.
`string.Utf8IndexOf(value, pattern, start)` performs an ordinal byte search and
returns its byte offset or `-1`. The start must be a valid scalar boundary within
the string; an empty pattern matches the start, including the end. Strings retain
existing UTF-8 validation, embedded-NUL support and their 1 MiB value ceiling.

`string.FromInt(value)` formats signed64 as invariant ASCII decimal without a
locale, grouping or leading plus, including the minimum signed64 value. All four
operations are pure, evaluate arguments once from left to right, and introduce no
I/O, host-service calls or implicit numeric conversions.

## Synchronous injected host calls

An injected `public Interface` may declare effectful synchronous
`Result<T,string>` methods alongside existing `Task<Result<T,string>>` methods.
The synchronous method is called normally; the async method still requires a
source `await` and retains its bounded FIFO host-worker behavior. A synchronous
host call executes directly on the model owner thread, without a task, worker,
suspension or implicit transaction commit. Pure model methods, parallel bodies
and helpers reachable from parallel workers cannot call host services.

Host parameters and Result payloads remain recursively owned values; existing
explicit cancellation-token parameters remain available. Source object
references, services and shared synchronization/collection handles cannot cross
this boundary. `source Interface` still means a source-implemented, closed object
contract, not an injected native adapter.

The generated call captures its service and evaluates each argument exactly once
from left to right, validating owned arguments before invocation. The runtime
validates the complete returned Result, including Failure/Cancelled errors.
Synchronous callbacks cannot reenter the same model through its public actions,
queries, snapshots or polling. Calls into another independent model remain
permitted. The callback guard is owner-affine, move-only and exception-safe;
destroying the model during its callback is a contract violation that terminates
rather than invalidating an active invocation. Callbacks must be bounded and must
not retain borrowed native scene state beyond the adapter's declared lifetime.
Service ownership remains constructor-fixed `std::shared_ptr<Interface>`; a native
adapter may scope a borrowed environment to a synchronous invocation without
exposing raw references in PipeLang.

Native adapters must leave the caller's floating-point controls unchanged on
both normal and exceptional return, including rounding and platform subnormal
handling controls. Existing `float32` invocation/poll admission guards remain in
force. The runtime does not write or restore floating-point modes or check every
arithmetic operation after a native callback. This boundary executes trusted
native C++; it provides no isolation from native code that violates its owner,
lifetime, bounded-work or floating-point contract. Concrete adapters should
qualify their control preservation before and after representative calls.

Returned Success/Failure/Cancelled outcomes are source values: an authored normal
return commits model state regardless of outcome. Host exceptions become source
`ExecutionFailure` faults, so normal source `catch (Error)`/`finally` recovery
applies. Thrown `OperationCancelled` remains cancellation, runs source cleanup,
and maps to a Cancelled action outcome when uncaught. An uncaught fault or thrown
cancellation rolls back the current model transaction; it does not undo external
host effects. Immediate effects remain visible to subsequent host queries in
source order. There is no effect buffering, distributed transaction or automatic
retry. Explicit cancellation tokens are cooperative; a synchronous callback has
no worker or hard interruption mechanism. Existing async commit/refresh points and
authoritative completed host outcomes are unchanged.


## Explicit one-time initialization budget

`pipelang::model::TaskOptions::initializationSteps` selects a generated root
constructor's normal evaluation allowance. It defaults to 65,536 steps. A host may
explicitly select 1 through 1,048,576 steps for bounded one-time setup; zero or a
larger value is refused before source field initializers or constructor work.
The installed SDK manifest pins this runtime/compiler API together. Existing
`TaskOptions{capacity}` aggregate calls retain the default allowance.

`EvaluationBudget::forInitialization(requested)` sets only the initial normal
allowance. Source field initializers, constructor helpers, nested ordinary object
constructors and their field initializers share that same outer budget. They do
not receive fresh allowances. Parallel child budgets share its remaining normal
counter; cleanup remains a separate shared 65,536-step allowance. Reset retains
the existing 65,536-step normal and cleanup defaults.

Public queries and actions start independent default 65,536-step invocation
budgets after initialization, regardless of the constructor's selected allowance.
Async suspension/reset behavior, cleanup limits, collection ownership bounds and
native process/cgroup containment caps are unchanged. This is explicit bounded
initialization policy, not a way for source code to raise or reset its limits.

Stored object-owned `List` reads keep the receiver's object root alive through
`Count` or a call-free `At` index. They inspect the stored count or decode only
the selected element into an owned value; stored object edges and borrowed
references never escape. Record-member list fields use the same path. An index
containing a call retains the full owned receiver snapshot before evaluating the
index, so replacing or appending to the field preserves evaluation order and
transaction rollback. Every source evaluation tick remains in its original order.

The evaluation budget has a small inline local-counter path. Cancellation,
shared counters, cleanup and exhausted counters use the existing outlined path;
this does not change work limits or combine source ticks.

### Pure synchronous host queries

An injected host interface can explicitly declare `pure Result<T,string>`
synchronous queries. Their native virtual methods are `const`. Pure source
methods can call these declarations and return Failure or Cancelled deliberately;
effectful host methods remain forbidden in pure code. All host calls remain
forbidden in parallel workers, including through helpers. Declared host purity is
a trusted native contract, not an isolation guarantee: `const` does not prevent
native I/O or mutation outside the authored model.

Queries retain the synchronous host boundary: exact owned argument/result
validation, owner-thread execution, and same-model public action/query/poll
reentry refusal during the callback. Existing effectful synchronous and async
host virtual signatures are unchanged.

### Typed IEEE values and string prefixes

`float` and `float32` expose `PositiveInfinity`, `NegativeInfinity` and `NaN`.
These are typed IEEE values; the named NaNs have positive quiet-NaN bits
`0x7ff8000000000000` and `0x7fc00000`, respectively. Arithmetic NaN payloads are
not promised. Existing finite literal admission, explicit conversions and
per-operation float32 rounding remain unchanged.

`string.StartsWith(value, prefix)` is a pure operation requiring two strings.
It compares their exact bytes, including embedded NUL and UTF-8, without locale,
case folding or normalization. The empty prefix matches every string.

### Source constants and the native constexpr boundary

A nongeneric class can declare public or private implicit-static scalar constants:

```text
public const int Gates = 18;
private const float32 Pi = 3.14159265358979323846f;
public const float32 Turn = 96.0f * Pi;
public const float32 Length = 624.0f * 2.0f + Turn * 2.0f;
public const float32 GateSpacing = Length / float32.FromInt(Gates);
```

The admitted types are bool, int, uint32, float and float32. Initializers use
closed scalar constants, operators, conditionals and explicit numeric conversions.
State, arbitrary method calls, dependency cycles, mutation and implicit numeric
mixing are rejected. Dependencies and evaluation depth are bounded. Integer and
uint32 operations use the existing checked rules; float/float32 operations retain
IEEE classes, signed zero and nominal-width rounding. A declaration performs no
runtime or initialization-budget work. Each authored read still charges its
ordinary literal/access expression tick; the closed initializer is not metered
again at every read.

`StandaloneConstants.hpp` exports public constants under
`<binding namespace>::constants::<source module components>::<source class>`.
An unmodular class is directly under `constants`. The leaf contains only standard
C++20 headers and constexpr values, with no model-runtime or Qt dependency.
Model headers remain self-contained and can coexist with the leaf. Public model
facades also expose constexpr aliases. Private constants remain source-private
and do not enter the export inventory or mutable snapshots.

The model manifest records each source identity, stable C++ identity, scalar
type, value encoding and exact bit/value string. Floating encodings store IEEE
bits, avoiding JSON NaN/Infinity ambiguity. The SDK tool publishes the leaf
atomically and hashes it in `bindings.json`; the CMake model function declares
it and exposes its path as target property `PIPELANG_CONSTANTS_HEADER`. Native
constexpr APIs should alias these generated values rather than duplicate source
literals. The standard default evaluation and initialization limits are unchanged.

Object-domain collection avoids marking scratch and cell scans when the domain
has no live cells. Provisional allocations count as live until the existing
matching-generation destruction path removes them. Owner/closed checks, candidate
extra-root validation, root validation and transaction publication remain in their
original order. This changes no source evaluation or ownership limits.
