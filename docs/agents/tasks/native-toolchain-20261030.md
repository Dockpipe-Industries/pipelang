# Native compiler, SDK, tests and packaging migration

Objective `pipelang-native-toolchain-20261030` completed the required default
Linux path on 2026-10-31: compiler, SDK, build, tests and packaging use C++ and
DockPipe with no Go/Python toolchain dependency. This record does not authorize
publication, consumer edits, system installation or Git lifecycle actions.

Current acceptance, clarified by the owner on 2026-10-31: zero Go/Python source,
test/tool scripts and language dependency manifests in this repository, and no
Go/Python dependency in the required compiler/build/test/package/bootstrap path.
The earlier requirement to port all 116 historical profiles was superseded.
Unused research implementations/APIs are retired after caller audit; useful
regression inputs may remain as language-neutral data and C++ tests. Required
active behavior moves to C++ before its implementation is removed. The scope is
PipeLang; consumer repositories remain read-only.

Before retirement, all 1,149 authored tracked/untracked files were preserved in
`/tmp/pipelang-native-toolchain-20261030/retirement-snapshot-20261031/source-before-retirement.tar`,
SHA256 `9264194a6b5cf1ad88788e2fd43b7441e6b2d3a9672f3719e791088662a745be`.
Read-only HEAD/branch/staged/status anchors accompany it. This preserves inherited
uncommitted work outside the repository; the archive is not a build dependency.

The live caller audit identified the installed PipeLangModel C++ SDK in ForgePipe
Counter/settings examples; the optional material-admission integration still
requires `pipelang.cpp.float64-calls.v1` and its standalone compiler metadata.
The editor calls `pipelang check --in ... --format json --stdin` without a profile
flag. No PipeLang dependency was found in the MesaMoto source/config scan. These
are verified local callers, not a claim that every external consumer was searched;
the DockPipe source repository was not accessed. Preserve these active boundaries
and remove unused Go/stream/application-IR research entrypoints from the new
required path rather than recreating historical capability cuts.

The starting inventory contains 87 non-test Go implementation files under src/sdk
(excluding generated compatibility fixtures) and 86 Python files under tooling,
tests, workflows and sdk. The machine inventory beside the research record owns
exact paths; counts are a baseline, not a completion metric.

| Boundary | Native owner | Current gate |
| --- | --- | --- |
| SDK manifest/bindings | src/native/sdk-tools | Exact installed manifest, bounded tools, transactional outputs and refusal tests |
| Containment | src/native/containedexec | Nine independent failure probes and unchanged resource/lifetime limits |
| Configuration CLI/materializer | src/native/legacy-compiler | Frozen configuration digests, editor default CLI, diagnostics and events |
| Active scalar frontend | src/native/scalar-compiler | Explicit float64-calls profile, executable hashes and exact metadata |
| Ordinary models and views | src/native/model-compiler, model-view-compiler | C++ compiler assertions, neutral data and generated runtime/sanitizer fixtures |
| Native SDK package | workflows/release | Checksum-bound archive/extract and relocated native tools/CMake consumer |
| Defaults/CI/bootstrap | workflows and tooling/bootstrap-dockpipe.cmake | Native source bootstrap, exact DockPipe lock and no language fallbacks |
| Historical Go APIs/stream/research | retired | Verified external source snapshot; no executable replica promised |

The original matrix below is historical checkpoint evidence. The owner scope
revision and current native defaults above take precedence.

The migration is not complete while Go/Python files or default workflow fallbacks
remain. Active contracts above require native replacements; historical research
profiles and Go import APIs need retirement records rather than executable ports.
No consumer repository mutation is authorized. Editor Node tooling remains an
independent implementation language, but its Python orchestration must be removed.

Preserve exact SDK pins, legacy v1/v2 bytes, explicit profile admission, owner-bound
identity/lifetime, transactional graph/root publication, async cancellation and
headless isolation. Bootstrap builds use the previous verified containment until
the C++ replacement independently qualifies. Required caps stay 1 GiB per unit,
800 MiB proactive, 700 MiB memory.high, zero swap and 2 GiB aggregate; the new
proof estate is capped at 8 GiB. Partial proof is retained.

Inventory: the native scanner records 87 Go implementation files, 260 Go test
files, 86 Python files, 29 compatibility fixtures and 116 legacy language profiles
in `docs/research/native-toolchain-inventory-20261030.json`. It also records the
seven command surfaces and five release targets; pending rows are not retirement
claims.

SDK/containment checkpoint: passed locally on Linux. The declared DockPipe build
workflow can select `PIPELANG_BUILD_NATIVE_TOOLCHAIN=1` with a fresh absolute
`PIPELANG_NATIVE_TOOLCHAIN_PROOF` and an independently qualified absolute
`PIPELANG_NATIVE_RUNNER` plus exact `PIPELANG_NATIVE_RUNNER_SHA256`. This path uses
native manifest, binding and containment tools. Python discovery is disabled in
SDK configuration and the installed CMake consumer. Its independent C++ oracle
passed 22 integrity/schema/transport/publication refusal cases and installed
compiler/consumer checks. Legacy installed Python helper files remain reference
compatibility inputs until their dependent test lane is migrated; this native
path does not execute them.

The C++ containment oracle passed nine failure probes, including actual cgroup
verification, uncontained entry refusal, child failure, timeout, a detached-session
descendant, proactive memory stop, log capacity, immutable receipts, storage
preflight and cancellation. The cancellation probe exposed and verified a repair
to an outer-sampler identity race; failed proof remains retained. Resource limits
are unchanged. The native runner additionally records its executable and command
hashes; estate limits can be lowered for probes, never raised above 8 GiB.

`no-go-python-build-2/job.json` records successful configure/build/install,
inventory, C++ SDK tests and execution audits through DockPipe with Go and Python
unavailable on the selected tool PATH. Each workload is traced inside its unit,
and C++ audits reject Go/Python execution attempts and interpreter scripts.
`native-containment-final-2/acceptance.json` records the qualified runner hash.
The first checkpoint retained an explicit Python seed bootstrap. That native-lane
fallback was subsequently removed after the native SDK seed qualified below.
Native Windows transport and non-Linux containment qualification are pending.

Compiler checkpoint: `src/native/legacy-compiler` implements the explicitly
selected v0.0.0.1 configuration frontend, checking, evaluation and artifact emission
in C++. All four frozen digests match the unchanged 45-file corpus: source,
projection, artifact bytes and evaluation. Legacy named configuration schema
fields retain their null/empty placeholders; these are not modern object values.
The installed `pipelang-native check|compile|invoke` requires
`--profile v0.0.0.1`; it never delegates to Go. The oracle additionally exercises
23 semantic/resource refusals and installed CLI argument, stdin, output and
publication contracts. Structured diagnostic parity, operation events,
materialization and the other 115 legacy profile contracts remained open at this
checkpoint; the following checkpoints record their subsequent progress. The
default legacy command has not been replaced by a partial implementation.

Test checkpoint: `native-compat-2/job.json` passed through the declared DockPipe
test workflow. Three migrated C++ oracles recorded 78 named cases across model
protocol, fundamentals and iteration. Nine source runtime fixtures passed normal
and ASan/UBSan build/run pairs, including collections, parallel/concurrent work,
synchronization, objects and transactions. Thirty-two workload execution traces
passed native Go/Python audits. The nine sanitizer runs use the restricted PATH
without ptrace because LeakSanitizer rejects tracing; their normal counterparts
are traced. This is an enumerated subset, not replacement of the remaining
Python drivers or the incomplete full compiler campaign. An earlier launcher
failure was a shell variable collision; its partial proof remains retained.

Packaging checkpoint: `native-package-build-1` passed C++ deterministic USTAR
round-trip and malformed metadata/archive cases. The explicit
`PIPELANG_PACKAGE_NATIVE_SDK=1` path in the declared release workflow passed in
`native-package-1`: archive, independent CMake archive reader, checksum-bound safe
extraction, relocated installed SDK/CMake consumer and relocated frozen legacy
CLI oracle. Package metadata says `local-native-sdk-checkpoint`; this is Linux
amd64 SDK evidence, not a five-target CLI release. No artifact was published.
Legacy reference Python helper files remain checksum-covered archive contents
until their compatibility users migrate; no native packaging workload executes
them. Source bootstrap and default build/test/release retirement remain open.

The byte-ownership audit after these checkpoints found only seven intended
changes among the original 1,002 PipeLang files (four SDK CMake files and the
build/test/release launchers), plus the new native implementation, tests and
records. All original MesaMoto and ForgePipe file hashes still match their
captured baselines.

The expanded native inventory records 525 exported Go function/method/type
declarations and 1,279 Go test entrypoints. Its limits are explicit: nested/table
and generated cases, interface members, constants and behavioral contracts still
need owner-specific expansion. These are pending retirement rows, not C++ parity
claims. Five native C++ driver suites now have 111 named cases: protocol (32),
fundamentals (33), iteration (13), async composition (11) and failure/cleanup (22).
`native-async-compat-1` admitted the last two with normal and ASan/UBSan pairs.

`native-materialize-build-1` admitted 19 native materializer contracts: source
configuration/default/empty/duplicate roots, YAML type mappings (including aliases
and flow sequences), unique interface implementation, update/force behavior,
malformed/ambiguous/symbolic source refusals and source protection. YAML parsing
uses unmodified yaml-cpp 0.9.0 at commit
`56e3bb550c91fd7005566f19c079cb7a503223cf`, selected offline with per-file hashes
and MIT/component notices. No system dependency was installed.

`native-diagnostics-build-2` admitted ten structured/text check contracts,
including deterministic multi-file syntax failures, source UTF-8 admission,
UTF-16 editor columns and unsaved/inert checks. It fixed source admission being
misclassified as a lexical unexpected-character error. Full semantic code/message,
related-span and API parity remains open.

`native-source-bootstrap-1` built a fresh SDK and all native toolchain targets
from C++ source through DockPipe using the admitted native SDK archive pinned at
`145e13752553e5f453e367d199b2012ec654070479e5a6130ddc7bc540d2d1b3`.
The native helper has no Python fallback. The seed extraction and all build/test
workloads were audited; the task PATH contains no Go/Python. This is a local
native bootstrap, not a published artifact or fresh hosted-runner qualification.

`native-package-2` passed archive/read/extract, relocated SDK consumer, frozen
legacy CLI, materializer and diagnostic contracts. `native-operation-build-2`
then admitted ten compile/materialize operation-event contracts (IDs, timestamp,
JSONL append, success/failure, logging failure, escaping and inert check/invoke).
Its first attempt exposed a root-level materializer path normalization error;
the repaired attempt and frozen digests passed. Relocation of this last event
change remains to be rechecked.

`native-class-1` admitted the existing C++ class-inheritance oracle through the
new native launcher: 45 refusals (including six legacy selection cases), source
and module API/wire comparison, runtime and ASan/UBSan. Ten exec traces passed;
the sanitizer runtime used the restricted PATH without ptrace. Source/model
semantics were unchanged. The test launcher no longer invokes Python.

State: executing. Compiler/CLI compatibility, complete test migration, controlled
CI/bootstrap artifact availability and five-target packaging retirement remain
open. The Linux local source bootstrap has passed. Durable
objective, Git/byte anchors and proof are in
`/tmp/pipelang-native-toolchain-20261030/`. Existing class-inheritance acceptance
is admitted as a prior baseline; it is not a no-Go/no-Python claim.

## Zero-language retirement — executing 2026-10-31

Owner authority: remove unused implementations and port active consumers; the
all-116-profile proposal is superseded. Retired 478 Go/Python/module/obsolete
workflow files after verifying the external source snapshot. Exact deletion
identities are in the task proof's `retirement-snapshot-20261031/retired-files.json`;
ignored inherited binaries were moved to that snapshot directory too. No Git
mutation, consumer source change or publication was performed.

The default workflows now use native C++ tools with no fallback flags. Fresh
source bootstrap compiles only containment in a verified capped systemd unit,
then independently probes it. It does not need a historical SDK seed. C++ tests
replace active model source drivers with 171 semantic-refusal data cases,
module/object/interface generation and existing runtime fixtures, and native
view/style checks. Settings inspection and style-receipt assertions are C++.
Unused historical Go/Core/stream APIs and editor/Windows Python orchestration
were retired, not silently presented as ported capabilities. The editor source
remains; Marketplace publication and native UI are outside this required path.

Terminal default build/check/test/package and relocated-consumer proof passed.
The following acceptance supersedes earlier in-progress checkpoints.

## Terminal acceptance — 2026-10-31

The zero-language repository and required default Linux path passed. Removed
377 Go files, 91 Python files, two Go manifests, two ignored generated bytecode
files and six obsolete workflow files. All 476 authored retired files belong to
the verified external source snapshot; generated bytecode was disposable cache.
The unused editor publication dependency tree was also removed; the extension
source and dependency-free Node tests remain. The language-neutral retirement
record is `docs/research/native-toolchain-retirement-20261031.json`.

Evidence root: `/tmp/pipelang-native-toolchain-20261030`.

- `zero-source-default-final`: default build, fresh C++ source bootstrap with
  no supplied runner/SDK seed, nine independent containment probes, installed
  runner qualification, all compiler/SDK assertions and native execution audits.
  Active `bin/` command names resolve to the installed C++ SDK.
- `zero-default-test-1`: 175 successful contained units, 77 execution audits,
  111 base compiler cases, 236 extended model/view cases, 45 inheritance refusals,
  and 21 normal plus ASan/UBSan runtime pairs. This tests the exact byte-identical
  SDK manifest from the final source-bootstrap build; no runtime acceptance was
  inferred across a changed SDK identity. Sanitizer runtime executions are
  intentionally untraced; normal counterparts and compilation are audited.
- `zero-default-package-final`: deterministic SDK archive, independent archive
  reader, strict extraction, relocated CMake consumer, configuration CLI,
  materializer, diagnostics/events, scalar metadata/runtime and extended
  model/view oracles passed. Archive SHA256
  `76103c787c7502a888ebe78321a6dcc6f57a1f8aee2fdb29c6504b1778f474e9`.
- `active-consumer-final`: actual ForgePipe MaterialAdmission.pipe read-only
  input passed all five exact signatures/metadata/hash contracts, compiled runtime
  checks including selected NaN payload/signed zero, and native-only exec audits.
  Ported settings assertion code compiled. The checksum-locked DockPipe 0.6.33
  download/extraction/version bootstrap passed through CMake without Python.
- `source-preservation-final.json`: HEAD, branch and staged paths unchanged;
  all retired authored source belongs to the sealed snapshot. MesaMoto's 142 and
  ForgePipe's 259 original files still match their original SHA256 baselines.

The source bootstrap, test and package traces attempted no Go/Python execution;
Go/Python were absent from task PATH. The strict repository gate also passes
with zero language source/bytecode/manifests and no workflow/CMake language
fallbacks. Earlier partial failures are retained, including the empty-cache
directory refusal that the zero-file gate correctly caught. Total task proof
remains below the unchanged 8GiB estate cap.

Scope limits: local Linux/headless and installed-SDK acceptance. Native
interactive UI, other-platform containment/release qualification, hosted CI,
signing and publication remain separate. Historical Go APIs and unused profiles
were retired by owner decision rather than claimed as native support. No
consumer repository edit, commit, push, dispatch or publication occurred.
