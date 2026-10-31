# Explicit native model compiler

`pipelang-modelc` is an additive C++20 ordinary-model capability. It does not
select, replace or modify the legacy Go/Core/profile compiler paths. Build this
CMake target explicitly; consumers admit the executable's exact bytes before
invoking it. The compiler uses the existing native lexer and pinned nlohmann JSON
header. It emits typed C++ and a manifest, not a private application opcode list.

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
NaNs; int/float conversions remain explicit refusals. `float.IsFinite`, `Abs`,
`Sqrt` and `Log` are pure numeric operations; domain errors follow the host IEEE
math domain, not integer checked-arithmetic rules. Strict default floating modes
are required; fast/finite-only compilation is refused. Signaling NaNs, nondefault
rounding/flush modes and cross-platform math-library bit equality are unqualified.

Binary32 is the explicit ordinary-model numeric primitive `float32`, emitted as
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

The legacy lexer entrypoint retains its grammar; only modelc explicitly selects
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

Bounds are part of admission: 64 KiB source, 4 MiB transport, 32,768 tokens,
16,384 expression nodes, 128 declarations, 256 members, 32 parameters, bounded
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
