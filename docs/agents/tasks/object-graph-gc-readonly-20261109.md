# Object-graph collection on unchanged read-only commits

Status: implemented and locally qualified in SDK7; no default/pin/release change.
Authority: the user explicitly approved the separate GC proposal on 2026-11-09.
No default SDK, dependency pin, publication or deployment change is authorized.

The source DI benchmark found one 65,536-byte requested allocation and roughly
10.3 microseconds per guarded read-only call in both equivalent manual/generated
lanes. `ObjectDomain::collect` reserves its bounded marking stack on every commit.
The unchanged SDK6 control is
`/tmp/pipelang-composition-factory-20261108/sdk-6`, manifest
`5f426dd3504d98b6d481ec608fd9ea8d7771a764791637fdf4ac95110df47df8`;
object.hpp SHA256 `fdc33bfeda56718ce522236f0fcfc3a20e27ea4589a4bf1af93a6750c9f00f07`.

An empty transaction alone is insufficient reason to skip collection: native
reference release, edge publication, replacement or removal of overlay/savepoint
versions, rolled-back provisional parents and temporary candidate roots can all
change reachability without new source object edits in the next transaction.
The approved design tracks those changes conservatively. Actual graph edits and
allocations still collect; only an unchanged, graph-empty operation with no pending
reachability change skips the redundant trace. Validation, prepared state and
native root publication remain unconditional. Explicit collection and resource
pressure still trace, collector failures restore pending work, and release events
during sweep remain pending. Supported owner-thread reclamation timing is preserved.
Foreign-thread reference use and arbitrary reentrant destructor operations are not
newly supported.

The initial broad follow-up mutation was rejected by automatic approval review.
The partial owning edit was restored exactly before a concrete complete diff and
witness plan were presented. The user then explicitly approved separate GC work.
Historical proposal/rejection/restoration evidence remains under
`/tmp/pipelang-root-set-gc-20261109/proposal`; no incomplete header was built or
selected before approval. The current implementation also includes independent
review findings such as overlay replacement invalidation.

Qualification must cover allocation-free warmed reads, the first pending sweep,
root-state-only publication, root/edge release and stale generations, cross-
transaction overlays, savepoints/discard/rollback, temporary extra roots and failed
collection recovery. Run the prior object-runtime normal/sanitizer oracles too.
Then build a fresh SDK and rebuild all generated/source-object consumers against
its headers: the private ObjectDomain layout changes. Preserve the old SDK and
all raw timing/outlier samples. Compare identical source/flags and equivalent
manual/generated ownership, separately measuring pending and steady reads.
Do not claim a collector or DI speedup until measured.

Owned-resource cleanup and scoped async DI remain separate lifecycle work.

## Qualified local candidate

Status: implemented and locally qualified; no default/pin/release change.
Candidate SDK7: `/tmp/pipelang-composition-factory-20261108/sdk-7`.
Manifest SHA256: `092564bf9469f7d5b192cf181d782b48dc9602dc536d6fee008f4d64e1a1ba48`.
Object header SHA256: `7606ac42315fe7834c7bf873a3f7a572cd21f9e9da3696927a0942d74f3c1195`.

- `/tmp/pipelang-root-set-gc-20261109/proof-3`: unchanged SDK6 baseline focused
  37 assertions and candidate 2,085 assertions, both normal/ASan/UBSan; established
  object_runtime also passes normal/sanitizer for both. Exactly one runtime header
  differs. 61.59 seconds, about 548.5 MB peak, no memory events.
- SDK7 `proof-7`: maintained GC 2,085, lifecycle 16,458 and execution 43 assertions
  pass, plus 47 installed SDK checks; all runtime consumers rebuilt. 13.35 seconds,
  321,241,088-byte peak, no events. Compiler/SDK tool binaries remain unchanged;
  compiler/tool object dependency inventories do not include object.hpp.
- `/tmp/pipelang-di-gc-qualification-20261109/qualification-job.json`: fresh SDK6
  compiled control and SDK7 header/compiled/compiled-sanitizer lanes each pass
  11,134 integration assertions, independent owner threads and namespace-shadow
  consumers. 214.55 seconds, 753,344,512-byte peak, no events. All 23 generated C++
  inputs match; every source-object consumer is rebuilt against its runtime.
- Matched benchmark and separate pending/steady probes pass. The additional fixed
  ABBA order (6,7,7,6) uses unchanged binaries/flags, keeps all samples and confirms
  generated read medians 10,199.4 to 277.9 ns/op (about 36.7x), manual 10,261.6 to
  279.5 ns/op. Both versions allocate 65,536 bytes on the first pending read; SDK7
  allocates nothing on subsequent unchanged reads. Releasing scope handles schedules
  the required first sweep again. Cached resolution remains zero-allocation.
- The same ABBA packet retains a construction tradeoff: generated transients
  34,174.8 to 36,174.9 ns/op (+5.85%); manual 33,431.7 to 36,176.5 (+8.21%), with
  overlapping ranges. First singleton/scoped construction changes are about
  +1.79%/+0.73% for generated wiring. No universal throughput improvement is claimed.
- Real Cloud access composition rebuilt normal/sanitizer in
  `/tmp/pipelang-cloud-di-20261108/proof-sdk7`: 336 source/native decisions each,
  fresh revocation, scoped ports, Result/token and closed-handle checks pass.
  29.44 seconds, 493,256,704-byte peak, no events.
- `/tmp/pipelang-cloud-di-20261108/wasm-sdk7`: fresh C++ compilation against SDK7
  runs the same composition as headless Emscripten 3.1.56/Node 16.20.0 WASM without
  pthread flags. Only previously built standard libraries are reused; no old
  source-object archive is linked. 6.35 seconds, 309,444,608-byte peak, no events.
  Qt UI/browser execution and async WASM are not claimed.

Full metrics/ranges, raw packets, retained harness fixture failures and exact
hashes are in `/tmp/pipelang-di-gc-qualification-20261109/REPORT.md`,
`comparison.json`, `abba-summary.json` and `packet-sha256.json`; independent review
is in `/tmp/pipelang-root-set-gc-20261109/independent-review/RESULT.md`.
No resource caps were raised. Linux x86_64 native and headless WASM evidence does
not qualify Windows/macOS/ARM or the unrelated dirty numeric work.
