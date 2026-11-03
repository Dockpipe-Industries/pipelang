# Pure synchronous owned-value host calls

A native host interface may declare a pure synchronous calculation:

```pipe
public Interface Suspension {
    pure float32 Support(float32 compression, float32 relativeSpeed);
}
```

The generated native override is `float Support(float, float) const`. The
compiler trusts the host's purity declaration; C++ constness does not prove that
it avoids external effects. Use `Result<T,string>` for expected refusals or
fallible callbacks. Effectful synchronous host methods still require Result;
asynchronous host methods still require `Task<Result<T,string>>`.

Raw pure transport admits deeply owned values, including scalar, enum, record,
collection and nullable values. Object references, service/shared handles,
cancellation tokens, void and raw Task returns are not admitted, including
references hidden in record fields. Argument and return validation uses the
existing owned-value runtime. Nullable absence stays a successful raw absence;
no missing, invalid or failed callback is replaced by a default value. IEEE
binary32 values, including signed zero and NaN payloads, retain their words.

Calls use the existing synchronous owner guard, argument evaluation order,
validation, Source instruction accounting and transaction boundary. Reentry into
the same owner (read, mutation or capture) is refused while a callback runs;
independent owners may nest. Foreign-thread entry is refused. These calls do not
create a task scope per callback. External host side effects cannot be rolled
back by Source transactions.

OperationCancelled stays cancellation: direct raw entries throw it, and an
uncaught cancellation in a Result-returning Source action retains its message
and rolls back Source state. Standard exceptions become ExecutionFailure with
the existing safe message; nonstandard exceptions use the existing generic
synchronous-host failure message. No error cause is inferred from a string.

Hosts must preserve the floating-point environment on every path. For models
using float32, normal raw-pure return checks the binary32 environment before
Source continues. Throw/cancel remains the primary outcome even if the host
also disturbs FP; no catch-path postcheck overwrites it, and no environment is
automatically restored. A later entry rejects the bad environment until its
owner repairs it. This capability does not add effect analysis or checked
nullable flow dominance; existing nullable Value access remains runtime checked.

The old synchronous Result emission and all runtime headers are unchanged.
Header-only and compiled project modes share the new call lowering. Both modes
passed focused local Linux normal and sanitizer checks, and the qualified SDK's
303-unit default campaign and installed/relocated consumer checks passed. The
qualified manifest is
`54465c14d9f65c9f1c00f426f2ebcfc1e9a3ae42a262b9afc6225b06722abaf0`.
Header-only remains the production default; compiled mode is experimental.
Windows, macOS and release qualification remain separate.
