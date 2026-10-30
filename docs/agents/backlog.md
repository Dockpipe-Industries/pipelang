# Backlog and migration direction

## Language direction

Agreed direction, renewed 2026-10-30: PipeLang is a modern object-oriented
language with familiar C#/Java-style readability and expressiveness, designed
for humans and AI. Mature C# features such as foreach/iteration, generics,
interfaces, pattern matching, lambdas, async/await and structured resource
disposal are design references to evaluate with judgment. This is not a mandate
to clone every feature, and this direction does not claim those features are
implemented or authorize their implementation together.

Real consumer needs should drive coherent, broadly useful language capabilities.
When a missing feature would materially worsen application code, prefer defining
and implementing that capability over special cases, duplicated logic, manual
unrolling or awkward helper methods whose only purpose is fitting the current
subset. Preserve cohesive application responsibilities. Bounded compiler slices
are verification steps toward that language, not its destination or a reason to
accept poor consumer architecture.

AI-first means clear semantics, strong types, explicit effects and failures,
useful diagnostics and inspectable behavior without sacrificing code that humans
can read and maintain. As real consumers need async/parallel work, define task
ownership, cancellation, failure propagation, cleanup, bounded concurrency and
result ordering together. Scheduling syntax alone is not a complete capability.
General language/foundation owners provide these semantics; UI and engine
adapters consume them instead of inventing private substitutes.

New compiler/runtime capabilities should be implemented in C++ first. Avoid new
Go implementations whose only consequence is another port later. Existing Go
remains the reference for already implemented behavior; changes to shared
semantics and IR remain deliberate. Use independent correctness oracles and
consumer evidence for genuinely new behavior rather than automatically building
a duplicate Go implementation. Continue preserving frozen compatibility,
resource bounds, explicit backend selection and fail-closed admission. Capability
records must distinguish planned support from implemented and qualified support.

The [2026-10-30 consumer architecture audit](../research/pipelang-consumer-architecture-audit-20261030.md)
found existing action/model and schema-copying debt. Its typed action bridge,
generated snapshots/event entrypoints, shared property/status/error projection
and typed material helper calls now have focused native/installed-consumer proof;
see [the native remediation record](../research/pipelang-native-models-20261030.md).
Terminal root compatibility checks passed; the consumer audit is closed at the
recorded focused Linux/headless/installed-SDK scope. This does not claim broad
language, graphics, platform or full-suite acceptance.

## Native migration and current evidence

The ecosystem direction is a gradual C++ rewrite over time across Dockpipe,
PipeLang and the package ecosystem. This is incremental work, not a claim that
the current Go implementation has been replaced.

The intended end state is a native compiler and build path that does not require
Go for ForgePipe application delivery. The lexer, bounded view parser and bounded
C++ emitter are already explicit native components; ordinary source parsing,
semantic/type analysis, IR lowering and compiler/SDK orchestration still have Go
owners. Port these responsibilities behind defined contracts and equivalent
consumer coverage before retiring the reference path. Broader C++ language
coverage follows the coherent capability direction above and remains a prerequisite; this is not a completed migration.

Before moving any boundary to C++, define its public contracts and fixtures,
implement one coherent responsibility, prove source and generated artifact
compatibility, measure relevant resource costs and preserve the previous
implementation until equivalent consumer coverage passes. Keep native hosts and
language semantics separate; choose stable SDK interfaces for consumers.

Follow-up work includes publishing versioned Go SDK and CLI releases, qualifying
standalone distribution channels, running the complete contained compiler suite
in the independent repository, and measuring independent build times against
the extraction baseline. These remain separate from the source extraction.

The 2026-10-30 standalone compiler attempt completed 8,504 of 8,914 planned
units with no completed-unit failures before its storage guard stopped it.
The remaining 410 units are unqualified; this is not complete conformance or
accepted storage-budget proof. Retained attempts, the two narrowly repaired
harness issues and current limits are recorded in
[the first-consumer task record](tasks/forgepipe-first-consumer-20261030.md).

- Qualify the standalone release archives on supported native platforms, then add Homebrew and OS package registry delivery using the same immutable artifacts. Registry publication and signing credentials require explicit operator setup.


The first bounded C++ emitter port is implemented as an explicitly selected
experimental companion. Go remains the frontend and default/reference emitter.
See [the port verification record](../research/pipelang-cpp-emitter-port.md) for
matched generated bytes, contained value/trace checks and the installed ForgePipe
UI proof. Broader language, ABI, platform and performance qualification remain
follow-up work.

An additional explicitly selected
[binary64 comparison profile](../research/pipelang-cpp-float64-comparison-profile.md)
preserves IEEE host comparisons through both emitters. It qualified the initial
ForgePipe material-admission opt-in; default pilot output and engine behavior
remain unchanged. Installed default/Go/native SDKs matched 5004 public
material cases exactly. This does not add floating arithmetic or a general
floating-point backend.

The additive [binary64 conditional-value profile](../research/pipelang-cpp-float64-conditional-profile.md)
adds typed conditional values and returns over existing source/Core semantics.
ForgePipe's current material opt-in selects it for restitution minimum alongside
four existing admission predicates. It preserves first-operand ties and signed
zero; it adds no arithmetic or source-language feature. This checkpoint retains
the established Go emitter as an exact-artifact reference, not as a requirement
to duplicate future new language/runtime capabilities in Go.


The tokenization slice is implemented in C++ behind explicit selection.
The unchanged Go lexer remains the reference/default and the Go parser and
semantic pipeline consume the native token stream through a checked value
boundary. The lexer and emitter share one internal process transport; source
admission and language contracts remain owned by the existing compiler.

See [the lexer slice verification record](../research/pipelang-cpp-lexer-port.md) for acceptance evidence and limits.

The bounded PipeView source syntax parser is also implemented as an explicitly
selected C++ companion. Go retains token/foreign-AST admission, semantic checking
and default parsing. Source declarations, spans, diagnostics and generated
artifacts match the reference; selected process failures refuse without fallback.
See [the parser contract and evidence](../research/pipelang-cpp-view-parser-port.md)
and [the installed consumer checkpoint](tasks/forgepipe-first-consumer-20261030.md).
XML conversion remains on its existing explicit path; native parser selection
refuses XML in this slice.


The current Counter/settings consumers use the C++-first ordinary model SDK
instead of the old closed Set/Save/Revert runtime. The former source-v2/Go state
model remains explicit compatibility evidence. New models own their state,
transactional methods and one authored async/await Save, while generated bridges
own typed transport/projection. Structured host awaits were the original boundary. The additive
[fundamentals checkpoint](tasks/language-fundamentals-20261030.md) now owns
binary64, Optional/List values, lexical blocks, bounded iteration and local async
composition. The [structured failure/cleanup continuation](tasks/language-failure-cleanup-20261030.md)
adds owned Error, throw/rethrow and try/catch/finally, including awaited recovery
and bounded cleanup. Source resource disposal, asynchronous finally and broader
task orchestration remain unsupported. Consumer porting stays paused while
compiler work continues.

## PipeView: modular typed UI roadmap

Status: design direction agreed on 2026-10-28. A bounded PV-01 foundation now
introduces experimental Go SDK declaration and ForgePipe transport packages;
see [the first-slice record](../research/pipeview-package-boundary.md). The next
bounded slice adds an opt-in PipeLang view source profile and a small
settings consumer; see [the syntax record](../research/pipeview-source-syntax.md).
The typed action/Save slice now adds an explicit v2 profile and a PipeLang-owned
settings showcase; see [the Save record](../research/pipeview-settings-save.md).
Full PV-01/PV-02 acceptance remains pending.
PipeView is the PipeLang UI framework. The legacy bounded source profiles and explicitly selected native ordinary-model
bridge are implemented. Richer styles, general composition and remaining UI
roadmap syntax continue to be planned; they are not implied by this checkpoint.
Go remains the compiler reference while the bounded C++ port continues. This
roadmap does not promote the existing ForgePipe JSON UI prototype to PipeView.

### Package ownership and dependencies

Keep the compiler, standard libraries and optional packages in the PipeLang
repository, with explicit public APIs, dependency declarations and focused tests.
A shared repository does not mean a single mandatory runtime or dependency set.
Apply these boundaries as each slice is built; do not broadly relocate existing
code or create empty packages just to match this plan. Package names below are
provisional until the package/import and distribution contracts are selected.

| Proposed package or module | Responsibility | Dependency boundary |
| --- | --- | --- |
| `pipelang.ui.core` | Typed view contracts, component identities, bindings, actions, capabilities and semantic inspection contracts | Backend independent; no Qt or ForgePipe runtime |
| `pipelang.ui` | Public component/composition API, reusable controls and layout | Depends on UI core; no mandatory backend |
| `pipelang.ui.styles` | Typed styles, tokens, exposed component parts, states and explicit override rules | Shared by both authoring formats and adapters |
| `pipelang.ui.markup` | Optional XML frontend and conversion with declarative PipeLang views | Lowers to the shared typed view representation; no second UI runtime |
| `pipelang.ui.qt` | Qt rendering, generated typed state/action bridge, QML integration and Qt import interoperability | Explicit optional Qt dependency; preserves the existing Qt boundary contract |
| `pipelang.ui.forgepipe` | ForgePipe rendering/input adapter and capability mapping | Consumes versioned public ForgePipe SDK contracts; no engine implementation in this repo |
| `pipelang.ui.testing` | Mount fixtures, typed interaction APIs, deterministic scheduling and failure evidence | Shared behavioral contracts; backend-specific harnesses remain with their adapters |
| `pipelang.ui.tooling` | Development inspection protocol, diagnostics, preview and reload coordination | Development-only integration over shared compiler/runtime contracts |

These are responsibility boundaries, not a requirement for eight independently
released artifacts immediately. Keep public dependency direction acyclic. Share
contracts without duplicating compiler semantics, state logic or renderers.
General-purpose state, Result, cancellation, ownership and async semantics belong
in language/foundation or standard-library owners, not a private UI dialect.
UI packages consume those capabilities as their prerequisites become accepted.
Headless consumers must be able to build without UI packages or native backends;
selecting one backend must not implicitly require another. Runtime and language
compatibility requirements must be distinct from exact reproducible release pins.

### Design contracts

- Primary authoring is declarative PipeLang component syntax. Optional XML uses
  the same component/property/action types, styles, diagnostics and capabilities.
  Neither format is a more powerful mode. Existing Qt/QML authoring remains a
  supported integration path through the typed bridge; arbitrary QML is not
  promised to translate to ForgePipe or PipeView markup.
- Both view formats lower to one typed representation whose relationship to
  existing Application IR must be defined before introducing any new schema.
  Round-trip conversion covers the agreed declarative subset: components,
  properties, bindings, named action references, conditions, lists and reusable
  components. Application logic stays in ordinary PipeLang. Test semantic
  equivalence; perfect whitespace/comment preservation is a separate tooling
  decision. Refuse unsupported conversion rather than silently losing behavior.
- PipeView styles are their own language with familiar CSS/SCSS-like structure,
  nesting, theme tokens, component parts and interaction states. They are not
  SCSS-compatible. Type-check properties, units, tokens, parts and states;
  specify scoped application and explicit precedence. Avoid unrestricted global
  selectors and implicit cascade/specificity behavior. Specify bounded layout
  rules and backend capability refusals rather than silently approximating them.
- State and actions are strongly typed. Bindings are pure reactive reads;
  edits go through checked actions. Define invalidation, batching, subscriptions,
  view/model ownership, cancellation and thread dispatch explicitly. Fallible
  operations preserve Success, Cancelled and Failure outcomes. Do not infer a
  GC strategy or performance guarantee from the UI syntax.
- AI and test automation are first-class consumers of the same versioned semantic
  inspection contract: stable IDs, roles, accessible names, typed values,
  available actions, enabled/validation state and pending work. Automation uses
  real interaction/action paths. Inspection and automation access are explicit
  host capabilities, with sensitive values redacted and development endpoints
  excluded from production by default.
- Tests inject clocks, services and scheduling; await defined action and render
  completion instead of arbitrary sleeps. Capture semantic trees, action results,
  relevant state transitions, logs and screenshots on failure. Shared behavioral
  tests complement native input, focus, accessibility and rendering tests.
- Hot reload compiles and validates a candidate before applying it atomically;
  invalid edits preserve the working view and show diagnostics. Keep application
  state separate from disposable views. Preserve compatible state, stable control
  identity, focus and scroll; reconcile subscriptions and cancel retired view work
  without replaying side effects or accepting stale completions. State/schema or
  native ABI incompatibilities require explicit migration or a reported rebuild/
  restart. No blanket promise of arbitrary native-code replacement.

### Ordered delivery backlog

The bounded package foundation is implemented; the remaining acceptance work in
all items below is pending. Each slice needs a bounded contract, independent
acceptance evidence and updated capability documentation before expansion.

- [ ] **PV-01 — Package and dependency contract.** Define import/export visibility,
  package metadata, dependency resolution/version compatibility, optional native
  dependencies and distribution units using existing compiler/SDK mechanisms where
  possible. Map foundation prerequisites and decide shared view representation
  ownership. Prove a headless consumer does not acquire Qt/ForgePipe dependencies,
  and each adapter can be selected independently. Record public interfaces and
  ownership as modules are implemented.
- [ ] **PV-02 — Typed settings-screen vertical slice.** The bounded source profile
  and player-name/VSync/Revert consumer are locally implemented and verified;
  [the profile](../concepts/pipeview-source.md) defines their explicit limits.
  The original source-v2 Set/Save/Revert model and background worker remain
  compatibility evidence; their earlier records describe that historical profile.
  Current Counter and settings consumers use the C++-first ordinary model SDK:
  source methods own edits, validation, saved/draft snapshots and status, while one
  authored async/await Save handles typed host success, failure and cancellation.
  Generated snapshots and event entrypoints replace application schema copies and
  dispatch switches. Shared pure projections drive availability and inspection.
  Exact installed native consumers and lifecycle/sanitizer checks pass at the
  focused scope in the [remediation record](../research/pipelang-native-models-20261030.md).
  The percent stepper remains a percent control. Storage retains legacy v1 reads,
  v2 writes and atomic persistence. Interactive rendering, physical input, richer
  controls and broad language coverage remain unqualified.
  Continue primary view syntax
  and typed state/actions, enum choices, checked volume, resolution, VSync,
  validation, dirty state, Save/Revert/Cancel, single-flight async save and typed
  error feedback. Save validates and captures a draft snapshot. Test success,
  failure, cancellation, teardown and stale completion. Replace hand-authored JSON
  at the authoring boundary with compiler-checked components/bindings; retain any
  necessary serialized backend artifact as generated transport.
- [ ] **PV-03 — PipeView styles.** The initial native view-scoped typed style
  foundation now supports named themes, length/color tokens, inheritance and
  explicit overrides against the existing ForgePipe style profile; see the
  [capability contract](../concepts/pipeview-styles.md). Focused headless
  settings/draw-layout and compatibility proof is recorded in the
  [task](tasks/pipeview-styles-20261030.md). Full acceptance remains pending.
  Continue grammar, property/unit types, theme
  tokens, scoped reuse, explicit overrides, component parts, interaction states
  and layout capabilities. Prove small visual edits without changing actions or
  state, and clear diagnostics for misspelled/unsupported properties and states.
- [ ] **PV-04 — AI/testing contracts alongside the first controls.** Native
  tooling now has an explicitly selected [authored style descriptor](../concepts/pipeview-style-inspection.md)
  with typed resolved values and an atomically published SDK sidecar. Default
  response/artifact compatibility is retained; this is compile-time metadata,
  not an effective runtime style or complete semantic-tree acceptance claim.
  See its [qualification record](tasks/pipeview-style-inspection-20261030.md).
  The first
  managed-view inspection and typed Go widget-harness slice is locally accepted:
  default value redaction, checked action wiring, explicit completions and native
  conformance are recorded in [the inspection proof](../research/pipeview-inspection-testing.md).
  These modules stay compiler/renderer-free; no developer endpoint is enabled.
  The optional view compiler now exposes structured XML admission failures and a
  versioned JSON failure mode for automation; see the
  [diagnostic contract](../concepts/pipeview-markup.md#machine-readable-failures).
  Continue the
  versioned semantic tree, stable identity and typed interaction API with fixture
  clocks/services, deterministic completion and failure artifacts. Prove one
  settings behavior suite over the shared model and selected backend adapters;
  test refusal, sensitive-value handling and absence of development endpoints in
  production builds. Do not defer automation hooks until after control design.
- [ ] **PV-05 — Qt/QML and ForgePipe integration.** Generate checked Qt mappings for
  the settings types and actions; qualify value conversion, thread/lifetime rules,
  notifications and errors against the existing Qt boundary. Prove an existing
  QML screen and a PipeView-authored screen share the same settings logic. Qualify
  the ForgePipe adapter using the installed SDK and native visible settings UI,
  with explicit capability differences. Include keyboard, focus, accessibility
  and rendering checks; offscreen/model tests alone are insufficient.
- [ ] **PV-06 — Optional XML frontend and conversion.** The bounded settings
  profile now has an optional XML frontend and checked semantic conversion in
  both directions; see [the XML contract](../concepts/pipeview-markup.md). It uses
  the existing typed checker/runtime and explicit build-time format selection.
  XML syntax errors identify authored byte offsets. Typed semantic diagnostics
  now map to the authored element opening tag, retaining generated-source related
  context; attribute-level mapping remains pending. Expand to documented
  conditions/lists/custom components only with equivalence tests. Reuse all core
  styles, state/action and backend machinery. Treat Qt `.ui` import as a separate,
  explicitly scoped interoperability feature, not PipeView XML compatibility.
- [ ] **PV-07 — Transactional style/view hot reload.** Start with styles and
  declarative structure while retaining unsaved settings, compatible focus/scroll
  and live model identity. Test invalid edits, removed controls, subscription
  cleanup, pending actions and stale results. State which edits reload, migrate,
  rebuild or restart; report failures clearly.
- [ ] **PV-08 — Incremental logic rebuild and broader qualification.** Establish
  measured rebuild latency and resource baselines, then consider safe action-code
  replacement with explicit compatibility/lifetime constraints. Qualify supported
  platforms, packaging and release profiles separately. Production uses compiled,
  checked artifacts and does not require a development reload service.

Current acceptance records remain the C++ emitter/lexer records above. PipeView
planning does not change their qualification limits or select the next compiler
port slice. Use installed DockPipe for development workflows; leave its repository
alone. PipeLang/PipeView remain open source; optional adapters do not relicense
ForgePipe, MesaMoto or inherited third-party material.
