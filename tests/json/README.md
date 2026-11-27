# Strict JSON generated-source contracts

This installed-SDK consumer imports the real checksum-pinned PipeLang.Json and
PipeLang.Testing libraries. `JsonSuite` registers 41 source table cases and one
ownership/cancellation case. Expected parse/encoding/number/resource outcomes,
missing/null distinctions, member enumeration, typed queries, large array/string
queries and handle lifetime assertions are authored `.pipe`. The source ownership fixture explicitly uses host-selected two-document Readers.
The native fixture
supplies arbitrary bytes, adapts the real JSON service to its generated host ABI,
invokes the source suite and checks results/leaks. It is not a native schema or
qualification-policy interpreter.

`json_native_contracts` additionally exercises repeated PMR exhaustion/reclamation,
small-arena begin failure, mid-parse cancellation, fragment caps and every hard
limit, default eight-document capacity, sixteen-document configured capacity and
shared aggregate reservations across Readers. Failed parse releases the tree
reservation immediately while preserving exact captured input until close. Run all three executables, including `json_maximum_input_contracts`. Source cases deliberately append small inputs byte by
byte, splitting every multibyte/escape/number token boundary; large cases use
64KiB chunks. A 4MiB exact input, >1MiB string and 8193-element array retain source
transport limits unchanged. A writer/serialization roundtrip is outside this read
slice and is not claimed.

Configure against an exact locally staged SDK under the repository's qualified
containment runner (heavy builds sequential):

```sh
cmake -S tests/json -B /absolute/proof/json -G Ninja \
  -DCMAKE_PREFIX_PATH=/absolute/exact-sdk
cmake --build /absolute/proof/json --target json_contracts json_native_contracts \
  json_maximum_input_contracts json_string_index_contracts json_string_index_source_contracts -j1
ctest --test-dir /absolute/proof/json --output-on-failure
```

Enable `PIPELANG_JSON_SANITIZERS=ON` to rebuild the native reader from the
selected SDK's manifest-bound sources and instrument the generated consumer.
The SDK exposes `PipeLangModel_JSON_READER_SOURCE_DIRECTORY`; no source checkout
or sibling dependency is used. Native parser instrumentation is required along
with consumer instrumentation. Source files and the pinned third-party header
are installed through `PipeLangJsonReaderSources` and hashed by the SDK manifest.

The ordinary Unix and Windows test workflows register `json_library` in fixture
selection and run normal/sanitizer variants. The owning workflow still runs its
other compiler oracles; a fixture selection is subset evidence, not a full-suite
claim. Linux enables LeakSanitizer; Windows/macOS follow their existing native
sanitizer policies.

Linux x64 qualification passes all42 authored tests, native resource/lifetime
checks and an exact4MiB/2,097,152-node flat-array traversal in both normal and full
ASan/UBSan/leak modes. Measured maximal workload peaks were about523MiB normal
and590MiB instrumented, within unchanged1GiB/800MiB unit policy. These are local
measurements, not throughput or native Windows/macOS/WASM acceptance. Installation
and publication remain separate from private selected-SDK qualification.

## Decoded-string index qualification

Two additional CMake/CTest targets pass on Linux x64 in normal and full reader
ASan/UBSan/LeakSanitizer configurations:

- `json_string_index_contracts` exercises the native owning primitive: 4096/4097,
  admission+1, duplicates at full capacity, decoded escapes/NUL/case/normalization,
  ascending/reversed/alternating AVL insertion with independently observed
  logarithmic comparison bounds, values above 1MiB, cancellation before/during a
  long common-prefix comparison, and a witness forbidding postcommit polling.
  It also covers deterministic no-fallback arena exhaustion both before and after
  prior entries, invalid/foreign/stale/mixed handles, empty documents, repeated
  independent-arena cycles, document close/destruction and aggregate reservation
  failure across Readers followed by recovery.
- `json_string_index_source_contracts` instantiates the actual generated optional
  interface with `StringIndexAdapter` sharing a `ReaderAdapter`. Authored source
  owns selection, equality assertions, typed genuine cancellation and token-free
  close. The native fixture invokes 4097 separate public source calls; this proves
  ABI forwarding/storage, not a single authored traversal or a larger per-call
  execution budget. A whole authored large-input traversal with a selected finite
  execution-segment profile is a separate required consumer proof.

Build/run these targets in addition to the three existing JSON executables in
both normal and `PIPELANG_JSON_SANITIZERS=ON` configurations against a freshly
manifest-bound installed SDK. One authored large traversal, maximal-index/tree
coexistence and native Windows/macOS acceptance remain pending. Existing maximal
numeric-array reader proof is independent of maximal index acceptance.
