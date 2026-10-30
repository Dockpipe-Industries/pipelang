# Explicit native model compiler

`pipelang-modelc` is an additive C++20 ordinary-model capability. It does not
select, replace or modify the legacy Go/Core/profile compiler paths. Build this
CMake target explicitly; consumers admit the executable's exact bytes before
invoking it. The compiler uses the existing native lexer and pinned nlohmann JSON
header. It emits typed C++ and a manifest, not a private application opcode list.

Input is exactly one JSON object on stdin:

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

The grammar supports ordinary public records, enums, injected host interfaces and
stateful classes. Values include signed64 integers, bool, owned strings, nested
records, enums and recursively owned `Result<T,string>` (including void and
record payloads). Results preserve Success/Cancelled/Failure and expose checked
Value, Error and outcome predicates. Default Result is Cancelled with empty error.
Optional and arbitrary user-defined generics are explicitly unsupported.

Records have primitive/value-expression defaults, positional construction and
named `new Record { field = value }` construction; omitted named fields retain
defaults. Class fields have defaults, an explicit constructor or a generated
default constructor. Host interfaces are private shared-lifetime constructor
injections. Class fields are externally read through owned `snapshot()` copies;
only typed methods mutate live state. Default enum is its first declared value.

Blocks, single-statement conditionals, typed locals, assignments, returns, pure
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

Async methods use `async Task<Result<T,string>>` and directly await typed host
service calls in local initializers or assignments. They can call ordinary
synchronous helpers and perform multiple sequential host awaits. The compiler
emits C++ coroutines and immutable owned service arguments. Transactions commit at
await boundaries and successful source returns; resumption refreshes live state
before applying further source logic. Pre-await state is deliberately visible
while work is pending. General local async-method composition, loops, exceptions,
finally, inheritance, overloads, arbitrary references and implicit conversions
are refused. This capability does not claim those wider language features.

Bounds are part of admission: 64 KiB source, 4 MiB transport, 32,768 tokens,
16,384 expression nodes, 128 declarations, 256 members, 32 parameters, bounded
syntax/type nesting, depth32 acyclic local call graphs, at most 65,536 expanded
source evaluation operations per method, and depth32/256 expanded nodes per
record or model value layout. Work counting includes repeated callsites and both
branches, and applies to effects as well as pure getters. It does not depend on
declaration order. Strings require valid Unicode scalar UTF-8 and have a runtime 1 MiB per-value ceiling. Runtime task
capacity is independently bounded as described in the SDK runtime README.

Focused fixtures live in `tests/native-models`. Compiler execution, generated
compilation and native test execution must use the existing `containedexec`
`job.py` and `run.py`; these files are not permission to run outside containment.
