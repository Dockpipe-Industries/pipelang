# Ordinary-model compiler resource budgets

Ordinary PipeLang models and projects retain separate dynamically sized source
units. Source text is never concatenated into a synthetic file; token, syntax and
diagnostic offsets are `std::size_t` byte offsets within the original unit.
There is no independent 64 KiB file or closure ceiling. Explicit resource policy
bounds input storage, discovery, token allocation, semantic structure and output.
These are admission budgets, not source-representation or integer-width limits.

## Select a profile

The SDK CLI accepts an absolute JSON profile path:

```sh
pipelang-sdk-tool model-bindings --resource-budget /project/compiler-budget.json \
    --sdk-prefix /qualified/sdk --sdk-manifest /qualified/sdk/share/pipelang-model/sdk-manifest.json \
    --sdk-manifest-sha256 EXACT_SHA256 --model-compiler /qualified/sdk/bin/pipelang-modelc \
    --project-root /project --source-root src --entry Application/Main.pipe \
    --namespace application --output-directory /build/generated
```

The native compiler accepts the same object through the host argument
`pipelang-modelc --resource-budget-json '{"declarations":512}'`, before reading
its JSON request from stdin. No environment variable or application-specific
exemption changes admission. `pipelang_add_model` accepts
`RESOURCE_BUDGET compiler-budget.json`; it passes the profile to configure-time
project/source-set admission and build-time generation, and tracks the file as
both a configure and generation dependency.

A partial profile overrides named fields; omitted fields retain defaults:

| Field | Default | Accounted resource |
|---|---:|---|
| `input_bytes` | 4 MiB | Encoded native request and raw source input |
| `output_bytes` | 4 MiB | Encoded response including its newline, SDK information/receipts and generated file admission |
| `indexed_bytes` | 16 MiB | All discovered project source bytes, including unreachable files |
| `indexed_files` | 4096 | Discovered source files |
| `directory_entries` | 32768 | All entries visited during discovery |
| `directory_depth` | 32 | Discovery depth |
| `source_fragments` | 4096 | Supplied project units |
| `modules` | 128 | Logical module identities |
| `declarations` | 128 | Source declarations, exports and total specialized declarations |
| `tokens` | 32768 | Per-unit lexing allocation and aggregate closure tokens |
| `expressions` | 16384 | Per-unit and aggregate expression nodes |
| `generic_instances` | 128 | Closed generic record instances |
| `implementation_units` | 256 | Compiled implementation translation units |

Every value must be a positive integer representable by the host's `size_t`.
Unknown keys, duplicates, zero, negatives, floating values and overflowing values
are refused. The profile document itself has a separate 64 KiB metadata bound.
Counters check available capacity before addition. A refusal names the exhausted
field so the operator can inspect the workload and deliberately select a policy.
Larger budgets do not increase the external containment limits or promise that
all workloads at those budgets will fit host memory.

For example, a project with several hundred ordinary declarations can set
`{"declarations":512}`. A large module graph may additionally need `modules`,
`tokens`, `output_bytes` and, in compiled mode, `implementation_units`. Select
only the resources the workload requires; file organization remains independent.

`input_bytes` covers encoded JSON on the CLI and the sum of raw sources in the
in-process compiler API. JSON quoting and module/path metadata consume capacity,
so an encoded request can exceed the budget even if the raw text fits. The SDK
checks raw reachable input before building the compiler request, then enforces
its encoded transport size. Discovery uses `indexed_bytes` independently, since
unreachable project files still receive full lexical validation.

The C++ compiler/tooling implementation can use `ScopedResourceBudget` around a
compilation operation. It owns its validated profile, is thread-local, restores
an outer profile after normal return or exceptions, and does not modify another
thread. An existing `SourceProject` retains its admission policy for drift checks.
This internal C++ API does not introduce a new installed compiler-library ABI.

## Pipeline and receipts

Discovery sorts admitted files and reads each under the indexed-storage budget.
The module-header pass still lexes the full file, including unreachable files;
lexing stops before exceeding its token allocation budget. The SDK groups explicit
module contributions without rewriting them and walks reachable imports with an
iterative DFS. It detects missing imports, duplicate imports and cycles with
original paths and offsets. The compiler parses each supplied unit separately,
merges its AST declarations, resolves cross-file symbols, normalizes generic
composition, checks types/effects, and emits the selected artifacts.

Compiler import-cycle and reachability traversal is iterative too, so raising a
module budget does not increase an unguarded native recursion depth. This is not
an incremental compiler or cache: drift checks deliberately rediscover/read/lex
inputs, and a fresh compiler process reparses its closure. Source bytes move into
indexed fragments and JSON compiler units where ownership permits; protocol
serialization and some retained request copies remain.
Type-usage queries over that finalized AST are memoized within each Generator;
both positive and negative answers are reused without global state or AST edits.

SDK information and bindings receipts include `resource_budget.profile` and
`profile_sha256` (SHA256 of canonical expanded policy). An explicitly supplied
profile also records its absolute source path and raw `source_sha256`; its content
is checked again before publication. Changing the profile therefore changes
admission, provenance and CMake dependencies. Ordinary compiler response and
manifest shapes remain unchanged for default-policy accepted input.

## Remaining independent profiles and guards

Parser nesting, inheritance depth, checker work/node bounds, identifier/path
admission and runtime execution/owned-value budgets remain separate checked
contracts. A source path retains its independent 64 KiB identity bound; module
names retain their existing identity admission. The compiled emitter's preferred
64 KiB body grouping is a batching target, not an input ceiling.

This change covers ordinary-model/project compilation and SDK admission. The
standalone legacy configuration, lexer-protocol and view frontends retain their
own existing protocol/source profiles; their limits have not been removed by
changing ordinary-model policy. It also does not qualify a newer numeric emitter
against an older runtime: compiler, runtime, SDK helpers and tests must be selected
as one coherent candidate. See the [qualification task](../agents/tasks/compiler-source-scaling-20261108.md)
for frozen source/runtime provenance, test receipts and measured limits.

The preserved closed-source-set descriptor retains its independent64KiB metadata
profile. This is separate from source-file bytes; ordinary discovered projects do
not require that descriptor. Source and language-profile compatibility must still
be selected deliberately when packaging a compiler SDK.
