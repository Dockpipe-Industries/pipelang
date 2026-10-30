# Reproduce headless native model acceptance

`qualify.py` owns orchestration for the existing native-model evidence subset.
It uses the repository's `Models.pipe`, `semantics.cpp`, model protocol oracle,
view protocol/interoperability fixtures and seven SDK admission tests. No copied
temporary fixture or handwritten generated header is an input.

Use `--build-sdk` to qualify the local implementation. Three contained units
configure `sdk/pipeview/model`, build with `--parallel 1`, and install into
`OUTPUT/sdk`. The source-owned verifier pins and checks that installation's
complete manifest before executing its tools and again before acceptance.

From the repository root, with absolute paths substituted:

```sh
python3 -B /absolute/pipelang/tests/native-models/qualify.py \
  --source-root /absolute/pipelang \
  --build-sdk \
  --output /tmp/native-model-proof --cxx /usr/bin/c++
```

For external artifact qualification, replace `--build-sdk` with
`--sdk-prefix /absolute/installed-model-sdk --sdk-manifest-sha256 EXACT_SHA256`.
These modes are mutually exclusive. Installed mode tests that exact artifact
against current fixtures; it does not qualify the local compiler/runtime
implementation. The selected C++ compiler must be ABI-compatible with the SDK.

Both job receipt and proof directory must be fresh. The script
delegates to `job.py` with receipt prefix `OUTPUT-job` when no
aggregate exists. Under an existing aggregate it verifies and reuses that job,
then runs each workload through `run.py`; it never creates an independent nested
aggregate. Output/cache stay outside sources and external SDKs. Linux
user-systemd containment is required; missing
containment fails closed. `job.py` caps aggregate memory at 2 GiB; every workload
runs sequentially through `run.py` with 1 GiB memory, 800 MiB proactive stop,
700 MiB soft reclaim, zero swap and a 900-second unit deadline. Compilation uses
one compiler process at a time. No Go toolchain, ForgePipe SDK, UI or GPU is needed.

The default 512 MiB proof-estate budget is sampled during each unit and checked
between units. `--estate-limit-mib` may select at most 8192 MiB. This is a sampled
stop, not a filesystem quota. Failed/partial receipts and artifacts are retained;
choose a new output directory for retries. An interrupted coordinator is cleaned
up by the enclosing aggregate job.

Normal semantic/lifecycle execution and ASAN with leak detection run by default.
`--no-asan` explicitly omits the sanitizer pair and records that reduced scope.
The model protocol oracle includes the existing 25 semantic and four protocol
refusal families; view protocol and real model-to-view interoperability run
separately. Existing SDK tooling tests cover malformed receipts, tool selection
and output publication. No language semantics are implemented in this driver.

Compiler/runtime/SDK source and fixture hashes are captured before the build and
checked unchanged before acceptance, together with the selected compiler hash.
Before/after source inventories, SDK manifest identity and unit receipt hashes
are retained. `acceptance.json` exists only after all selected units pass and each unit receipt
reports complete process-tree removal. Check the enclosing job receipt too:
coordinator success alone does not prove aggregate teardown. This does not prove
the full compiler suite, generated ForgePipe adapter execution, visible rendering,
physical input, performance, other platforms or release readiness.

The style unit also checks typed declarations, all supported metric boundaries,
43 refusal families, explicit selection/overrides and generated model/action
separation. It emits the independent draw/layout project but does not execute
ForgePipe in this headless runner. See the native model-view fixture README for
exact installed-SDK execution and optional prior-compiler artifact comparison.
