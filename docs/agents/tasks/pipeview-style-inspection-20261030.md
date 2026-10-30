# Resolved authored style inspection

Objective: pipelang-pipeview-style-inspection-20261030
State: completed at focused Linux compiler/headless installed-SDK scope
Execution skill: dorkpipe-objective-execution
Authority: owner requested committing the verified style foundation and continuing
to the next piece. Local prerequisite commit: d1bb9f2. No push followed.
Checkpoint policy: automatic within this objective; handoff only on request.

Expose an explicitly selected, versioned and renderer-free descriptor of the
selected style's resolved authored properties. Build tools/automation consume
typed metadata rather than parsing generated C++ or application model state.
The descriptor distinguishes authored properties from omitted backend defaults
and host-owned scale. It makes no claim about later runtime style overrides.

The native request optionally selects `style_inspection` with the exact protocol
`pipelang.native-style-descriptor.v1`. Success then includes `style_descriptor`.
Default request/response/header/manifest behavior remains byte-identical. The
installed SDK's CMake helper opts in with STYLE_INSPECTION and atomically publishes
`style.json` with the ordinary generated outputs and receipt hashes. Strictly
validate the descriptor before publication; unsupported/malformed/mismatched
results refuse and preserve existing outputs. No fallback or new Go semantics.

Done when independent native metadata/refusal/default-compatibility tests, SDK
admission/rollback tests, exact installed settings generation and its existing
behavior tests, source-built regression with sanitizer coverage and declared
root checks/build/tests pass. Document the scope and update the roadmap.

Component-scoped rendering needs a coordinated versioned adapter seam because
current Session/layout/draw use one owned view style. This objective does not add
an adapter-local style map, change engine ABI, model semantics or control identity.
Preserve original branches/indexes and all unrelated bytes; ForgePipe/MesaMoto
remain untouched. No new commit/push/merge/publication, UI startup, costs,
credentials, installs, cleanup or full compiler campaign is authorized.

Use installed /usr/bin/dockpipe 0.6.33 explicit root workflows. Existing
job.py/run.py caps remain: 1 GiB unit, 800 MiB proactive, zero swap, 2 GiB
aggregate; sequential jobs and an 8 GiB new proof estate. Retain partial evidence.

## Accepted proof

Default native response bytes match the prior style compiler for both styled and
unstyled source. The opt-in response changes neither header nor manifest. The
independent descriptor oracle verifies resolved token overrides, canonical colors,
unselected and empty styles, omission of model/token/host values, and nine refusal
cases. Source failures publish no success descriptor.

All 15 SDK admission/rollback tests pass. The actual installed generator passes
four default/enabled/disabled/enabled modes with unchanged core bytes, hashes and
exact owned outputs. Invalid style source preserves previous complete bindings;
unknown user files refuse without overwrite. A headless CMake consumer passes
disabled/enabled/disabled rebuilds in one build directory with matching artifacts.
The settings consumer explicitly selects the descriptor and passes seven CTests,
including its independent descriptor/receipt oracle and six existing behaviors.
Its four ordinary generated artifacts match the preceding style consumer exactly.

Declared root check/build/test pass with all 15 source-built native regression
units and normal/ASAN/leak lifecycle checks. Installed consumer compiler binaries
match the final source-built tools. Independent review found no actionable issue.
All aggregate/unit process trees are removed; zero swap, no OOM and the unchanged
resource limits are verified. No engine or game source/ABI change was needed.

Evidence estate: `/tmp/pipelang-style-continuation-20261030/`; see
`commit-receipt.json`, `native-wire-parity.output`, `installed-generation.output`,
`cmake-integration.output`, `settings-test.output`, `consumer-job.json`,
`cmake-job.json`, `terminal-job.json` and `terminal-native-proof/acceptance.json`.
The preceding style foundation is committed locally as `d1bb9f2`; this inspection
continuation remains uncommitted. No push or publication followed. Original
branches/indexes and unrelated bytes are preserved; ForgePipe and MesaMoto are
untouched. Runtime styling, component/interaction-state adapters, visible UI,
GPU/input, hosted/cross-platform/performance and release proof remain pending.
