# Reproducible native model regression qualification

Objective: pipelang-native-model-regression-qualification-20261030
State: completed at focused Linux headless source-build scope
Execution skill: dorkpipe-objective-execution
Authority: owner requested committing the audited remediation and continuing with
its stated architecture and quality requirements on 2026-10-30.

The remediation is committed locally as `2c5b3f0` in the existing PipeLang
publication runtime checkout; ForgePipe integration is committed as `4984bfd`.
Neither was pushed. Both original dirty checkouts and MesaMoto were preserved.

## Bounded outcome

Make the already qualified native model protocol, generated semantic/lifecycle,
ASAN/leak, actual model-to-view manifest and SDK tooling checks repeatable through
source-controlled tooling. Support a headless invocation without Go or ForgePipe
and an explicit opt-in from the existing declared root test workflow. Reuse the
existing aggregate and unit containment; do not widen limits or trust environment
variables without checking the cgroup. Select and verify an exact installed SDK.
Keep generated artifacts outside sources and retain failures and receipts.

Done when the source-owned runner passes against the qualified exact SDK,
the root workflow integration and final check/test pass, SDK bytes and original
authored state remain intact, and usage and proof limits are documented.

No new language semantics, UI styles, publication, merge, release, native UI
startup, system install, complete compiler campaign, credentials or pruning.
Later UI roadmap expansion follows this regression checkpoint. The existing
experimental native capability and legacy compatibility profiles remain explicit.

## Accepted proof

The standalone source-owned runner passed all 12 sequential contained units,
including a freshly configured/built/installed SDK, 25 semantic and four protocol
refusal families, generated semantic/lifecycle tests, ASAN/leak execution, view
protocol/interoperability and seven SDK tooling cases. Source, fixture, compiler
and installed-manifest identities remained stable. The proof estate used less
than 8 MiB of its 512 MiB sampled budget. The source-only run invokes no Go or
ForgePipe tooling.

The declared root test workflow also passed with the opt-in enabled. This proves
reuse of the existing aggregate and unit containment alongside the existing Go
and frozen compatibility checks. Both aggregate receipts report successful
process-tree removal, zero swap and no OOM events; no limits were raised.
Independent review found no actionable issues in orchestration or scope claims.

Evidence: `/tmp/pipelang-commit-continue-20261030/` contains
`native-model-source-proof/acceptance.json`, `native-model-source-proof-job.json`,
`root-workflow-native-proof/acceptance.json`, `terminal-root-test.json` and
`terminal-job.json`. Check job teardown receipts as well as unit acceptance.
The preceding remediation commits remain local; this additional quality tooling
is uncommitted. UI/GPU/input, hosted CI, other platforms, full compiler conformance
and release qualification remain outside this evidence. Styles remain planned.
