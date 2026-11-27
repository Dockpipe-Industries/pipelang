# Ordinary compiler source scaling — 2026-11-08

Status: compiler and SDK locally qualified. The parent owns the final optimized
application-consumer refresh and overall task report. No publication or Git
lifecycle effects were performed. Preserve the ongoing numeric, DI and Cloud work.

## Root cause and architecture

The 64 KiB restriction was repeated admission policy in the ordinary parser,
module linker, SDK source-set loader and project closure loader. Storage already
used separate dynamic strings/JSON values and vector/map ASTs. Token, syntax and
diagnostic offsets were `size_t`; no fixed buffer, 16-bit offset or concatenated
source closure needed replacement.

The correction replaces that independent ceiling with a validated, named
`ResourceBudget`. Raw source and encoded requests share configurable input
accounting; indexed storage and generated output have their own budgets. See the
[canonical API and remaining limits](../../concepts/compiler-resource-budgets.md).
Selection is explicit: SDK `--resource-budget FILE`, native compiler
`--resource-budget-json OBJECT`, and CMake `RESOURCE_BUDGET FILE`. There is no
application exemption or environment override.

A scoped profile owns its values, restores outer state after exceptions and stays
thread-local. Counters check capacity before addition. The lexer stops token
allocation at its budget. Iterative import traversals preserve missing/cycle
locations without exposing native recursion when module budgets grow. Source
bytes move where ownership permits; full lexical validation of unreachable
indexed files remains. No incremental compiler or cache of source input was added.

SDK information/bindings receipts record the expanded profile, canonical hash and
optional original file/hash. Drift is checked before publication. CMake tracks the
profile at configure and generation time. Default accepted-source compiler
response/manifest shapes stay unchanged; other syntax/checker/runtime guards stay
active and are documented separately.

## Coherent source and SDK selection

The preserved original SDK is:

- Prefix: `/tmp/pipelang-native-resolver-csharp-pack-20261108/sdk`
- Manifest: `200890fd5a451f7e1f1665104dcedb8cce4491fac9c09a2f3d98b51bec6c5281`
- Model compiler: `5b2a6d315e0940f2d7a425669d9212688af21f49a89fc654262b25efdfbdc959`

An initial working-tree candidate passed the new budget tests but exposed unrelated
numeric-feature drift: its new standalone numeric header required constexpr
`Result` support absent from the carried qualified runtime. The current fixture
also expected `numeric_header`, which the old compiler did not emit. Bounded
errors and the old control are retained; neither runtime headers nor numeric
assertions were changed to conceal the mismatch.

The coherent candidate therefore applies task-owned budget changes to frozen
qualified source snapshots. The selected checker and original generator match
ByteBuffer qualification hashes exactly:

- Checker: `0a8ba537a70de098388eca51216003f6ba834977083b5b93dbe4ee050c48c310`
- Original generator: `8a617fc81861f466c7d8583bdd17c4128bea1b5e60abe7737744df9197c01682`

Matching earlier compiler, SDK helper, CMake and numeric fixture files are selected
with them from `/tmp/pipelang-color-convenience-20261108/language-before`. The frozen
closure and task patches remain in `/tmp/pipelang-source-budget-20261108`. Existing
working numeric feature code/tests remain intact. All prior qualified runtime
headers are carried byte-for-byte. Builds use matching `-O3 -DNDEBUG` flags and
unchanged containment limits.

The accepted budget-only SDK remains at `sdk-final`, manifest
`1d18f6002a51c90c81aa0e4c4ae7626dee5f5657d97063579dd2594d395e96f6`.
The selected measured optimization is a separate immutable candidate:

- Prefix: `/tmp/pipelang-source-budget-20261108/sdk-optimized`
- Manifest: `74d5034dc5b927eae7848f91ad933982a824d51e06537bdd512c0ba5fc321bad`
- Model compiler: `228aa81409c0577a0ddb8f1d38d97346036bd92e1db9ac5bda6b61bea373a009`
- SDK tool: `bf13871dc6601781ae48178c1061de30e00b42990e716647739914ad67ee78e5`
- CMake helper: `8631010509a96c9bda305506fefd3e8774b8ad8272df6182855e35eeef08e90f`

## Verification and measured performance

Both coherent candidates passed the complete matching SDK/tooling suite: 67
refusals plus installed, alias/discovery and incremental consumers. They also
passed 419 model/view oracles, 55 budget/correctness assertions and eight actual
SDK/CMake profile tests. The latter cover a low declaration refusal, a raised
policy emitting all 200 records, source/canonical hashes, automatic profile-file
dependencies and a later low-token refusal preserving prior accepted output.
The new regressions are registered in the owning Unix/Windows build scripts;
execution here qualifies Linux only. Frozen compiler goldens were not rewritten.

Independent stress passed all 31 cases, covering individual/closure boundaries
around 64 KiB, large files, hundreds of units, configured 200-record/class projects,
typed 128–1024 module chains, late correctness diagnostics, floods and repetition.
All 31 before/after first responses and 20 interleaved follow-up responses were
byte-identical after the optimization.

A task-only instrumented 1024-chain run attributed roughly 21.63 ms to parsing,
linking and resolution; 4.14 ms to normalization; 9.39 ms to semantic checking;
1656.16 ms to generation/manifest; and 23.74 ms to output admission. These are
attribution samples, not production benchmark timings. CPU sampling identified
repeated `Generator::usesType` whole-AST scans as the dominant cost.

The small fix memoizes positive and negative type-usage queries within one
Generator. The query reads only the finalized Program and type name, independent
of emission mode. It introduces no global cache or AST mutation.

Matched O3 interleaved medians:

| Nodes | Before wall ms | Cached wall ms | Before/cached CPU ms | Before/cached RSS KiB |
|---:|---:|---:|---|---|
| 512 | 462.480 | 75.018 | 456.884 / 73.820 | 19248 / 19272 |
| 1024 | 1823.435 | 193.739 | 1731.387 / 188.931 | 32908 / 32984 |

These workloads improved 6.16× and 9.41× in wall time with similar resident memory.
There is no strict linear-scaling claim. An earlier three-sample 512 batch had
outliers; it remains preserved beside the bounded interleaved follow-up.
SDK project-info controls measure verification/discovery/header lexing/reread and
hash checks together, not isolated I/O. No discovery speedup is claimed.

Proofs: `coherent-proof`, `final-accept`, `final-oracles`, `profile` and
`optimized-proof` under the compiler proof estate. Independent raw metrics and
response comparisons are under `/tmp/pipelang-source-scaling-20261108`, including
`noise-check`. Failed configure, namespace, template-assembly and provisional
numeric attempts remain separate and are not counted as acceptance proof.

## Consumer and qualification boundaries

The parent owns actual knowledge/Cloud qualification. Its default-budget knowledge
closure has 36 files, 11 modules and 66,078 bytes. Generation, native tests and
ASan/UBSan passed on the accepted pre-cache SDK. That sanitizer job recorded 88
memory.high throttling events, zero max/OOM events, with unchanged caps. Optimized
consumer refresh and byte-equivalence evidence are recorded by the parent. Cloud's
local policy module and exact-pin SQLite dependency pass normal/sanitizer checks;
emitted policy artifacts remain byte-identical across compiler optimization.

This does not qualify the complete Cloud UI/WASM product, remote sync, publication
or other platforms. Legacy configuration, standalone lexer and view profiles retain
their own separate limits. Other whole-program lookups and serialization remain;
large configured budgets do not promise unlimited host capacity. The task does not
release unrelated new numeric features or silently update the runtime.
