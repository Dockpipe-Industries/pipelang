# Native source projects

The opt-in native project compiler discovers `.pipe` files beneath a project source
root (conventionally `src`), indexes their existing `module` and `import` declarations,
and compiles only the selected entry's transitive import closure. Consumers declare
a project root and entry, not a list of files, module names or exported types.
An entry can contain only imports, selecting several independently owned models.
Local project discovery needs no project descriptor or package resolution language.
Optional external source libraries use explicit pinned descriptors below.

An explicit source composition root can select constructor bindings and service
lifetimes. See [source dependency composition](dependency-injection.md) for the
`Startup.pipe` syntax and opt-in `COMPOSITION_FACTORY` generation contract.

```cmake
find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED)
pipelang_add_model(world
    PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}"
    ENTRY tracks/world/World.pipe
    NAMESPACE game_world
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/world")
```

`SOURCE_ROOT` optionally selects a portable path relative to `PROJECT_ROOT`; its
default is `src`. `ENTRY` is a portable `.pipe` path relative to that source root.
An explicit `module` declaration determines source identity. Otherwise the filename
stem is the implicit module: `Point.pipe` can be imported with `import Point;`.
The stem must exactly satisfy the compiler's ordinary identifier rules; names such
as `bad-name.pipe`, `class.pipe` or `two words.pipe` require an explicit declaration.
Explicit modules remain authoritative even when the filename stem is not an identifier.
Folders do not contribute to implicit identity. Files with the same explicit module
contribute distinct declarations to that module, so one substantial type can live
in each domain-owned file without changing its qualified source identity. Imports
remain lexical to their own file: an import in a sibling file does not grant access
to that dependency's types. A module's dependency closure includes the union of all
its files' imports; importing or selecting any part selects all explicit parts.
Duplicate qualified declarations fail with both source paths and offsets; a type
cannot be split into partial declarations. Two implicit identities or an
implicit/explicit identity collision still fail with both paths, because an
implicit filename does not express deliberate module contribution. Domain folder
moves preserve explicit module identities; renaming an implicitly named file
changes its identity.
Keep one substantial authored type per file and arrange files by domain and role.

## Source model roots and ordinary objects

A transactional model root is declared in its source:

```text
module Racing.Race;
public model Class Race {
    public int laps;
    public Race(int initialLaps) { laps = initialLaps; }
}
```

The `model` modifier adds an isolated transactional/native owner to an ordinary
source class. The same source identity remains constructible with `new`, usable
through source interfaces, and usable as a base of ordinary source classes. Source
instances belong to their containing object graph; separately native-constructed
instances retain their own lifetime. Source `Interface`, host `Interface`,
`Record` and `Enum` semantics remain unchanged. Unmarked classes do not acquire
native owners. A class library may contain ordinary types without model roots.

The compiler derives both facets from the same normalized source declaration. It
retains the efficient direct native state/transaction lowering and emits the
ordinary object helper implementation independently. It does not allocate a new
native owner or task scope when a source object method is called. Private host
capabilities, generic helpers, constants and async/result flow remain source-owned.

Project native roots and ordinary object facets share small binary32 arithmetic helpers. A braced
operand aggregate evaluates the first operand before the second; abrupt completion
prevents later evaluation. Every helper retains a volatile binary32 result, so
nested Source operations keep their rounding boundaries even when C++ contraction
is enabled. This replaces repeated generated closure types, not Source methods:
all native/object entry routes, constructor bodies, dispatch and metadata remain
present. Direct native state, lifetime and transactional lowering are unchanged;
legacy-protocol output retains its original arithmetic closures.

Native owners still publish only owned-value snapshots and require public native
constructors. The current direct native representation explicitly rejects marked
classes that themselves inherit a class, or use an explicit `this`/`base`
receiver. These shapes need a coherent native receiver bridge; they are not
silently converted into disconnected state. Ordinary unmarked classes retain
existing inheritance and receiver semantics. Such a refusal names the native
owner facet rather than treating the source class as non-instantiable.

All public types in the resolved closure receive compiler-owned native names:
`game_world::source::Racing::Race::Race`, for example. Model roots name the generated
native model class, whose `State` and `snapshot()` are the existing owned snapshot
contract. A marked class's nested `Reference` alias names its ordinary source
object handle, for example `game_world::source::Racing::Race::Race::Reference`.
The generated nested name `Reference` is reserved for marked-class members.
Ordinary classes and source interfaces name their owner-bound `ObjectRef` types.
Host interfaces name the native abstract service interface. Records and enums
name their native value types. Consumers do not inspect private generated ordinal
names or maintain binding JSON/alias-generation scripts.

Generic records receive source-qualified alias templates for the closed applications
already instantiated by the source program. For example, `source::Data::Box<int64_t>`
can name the source `Data.Box<int>` specialization. This does not allow arbitrary
native callers to instantiate additional language specializations: those lack a
native specialization and fail C++ compilation. The compiler's existing closed
generic limits remain authoritative. Source type/module namespace collisions are
rejected rather than silently renamed.

Implicit module identity is adopted directly by the project linker; source text is
never prefixed or rewritten, and source diagnostic offsets remain unchanged. Legacy
v2/v3 closed-module requests continue requiring explicit matching module declarations.

The `model` modifier is recognized only in a project-mode declaration immediately
after `public`; existing protocols and ordinary identifiers named `model` retain
previous grammar and behavior. Legacy MODEL and explicit SOURCE_SET transports
remain available for frozen compatibility fixtures. Their generated output format
is unchanged; the new source-qualified native API belongs to project mode.

## Discovery, rebuilds and publication

The SDK uses the model compiler's lexer and module/import parser. No CMake or
consumer regex parser determines source semantics. Discovery sorts source paths
and rejects symbolic/reparse entries, unsafe paths, implicit identity collisions, missing
imports, duplicate imports and import cycles in the selected closure. Indexing
validates source lexing and module/import headers throughout the source root;
unselected declarations are not parsed, type checked or emitted.

Project roots and dependency paths must be representable literally by CMake's
source-discovery and dependency machinery. Project paths therefore reject glob
metacharacters (`[]*?`) and generator-expression openers (`$<`), alongside the
existing list/line separator and symbolic/reparse protections. This refusal is
project-local; legacy source transports and generic SDK path rules are unchanged.

An explicit [resource profile](compiler-resource-budgets.md) owns indexed storage,
discovery, transport, module/declaration counts, tokens, expressions and generated
units. It replaces the independent 64 KiB source-file/closure ceiling without
concatenating files. Default structural budgets remain checked and can be selected
deliberately for a larger application. Repeated module/import declarations in
separate physical files count toward those budgets. Source-path identity and
module-name admission retain separate bounds; execution limits are unchanged.
Compiled mode keeps complete definitions in separate implementation units rather
than requiring a larger application to become one giant header.

CMake watches directory membership with `CONFIGURE_DEPENDS` and consumes the
SDK-generated dependency inventory. Any indexed source edit, addition, removal,
module rename or import change causes admission/generation to run again. Rebuilds
may conservatively rerun generation for unrelated source edits; unrelated domains
remain outside the compiled header. Outputs must be outside the source root.

Generated `bindings.json` contains the bounded per-file index, source hashes, entry,
unique module closure and every contributing source path. The SDK rescans and compares inputs before publication; changed
source bytes, membership or imports reject publication. Existing pinned SDK/tool
verification and atomic output-directory ownership rules still apply. JSON here is
an internal transport/receipt, not an authored project or binding inventory.

The native tool's `project-info --project-root ROOT --source-root src --entry FILE`
returns generated dependencies. `model-bindings` accepts the same project arguments.
Both commands retain their pinned SDK prefix/manifest/hash requirements. The model
compiler's low-level `pipelang.native-model-project.v1` message contains the generated
closed source graph as per-file name/source/path records; no filesystem access is
added to that compiler process. Parsing merges declaration ASTs rather than
concatenating or rewriting source text. File-local offsets and actual origins
remain attached through linking, checking and generic normalization. Low-level
legacy requests do not acquire the project path field or multi-file behavior.

Projects resolve local modules under one source root plus explicitly provisioned
pinned source libraries. Package acquisition/version resolution, binary module linking, partial type declarations, and
project-mode view bindings remain unsupported. Native consumers still pin installed
SDK artifacts independently. A source project is not a package manager or an ABI
promise across SDK versions.

## Explicit pinned source libraries

A project may additionally admit source libraries that Dockpipe or another host
has already provisioned. Acquisition, credentials, archives and version selection
remain host responsibilities. This SDK feature resolves source modules from exact
local inputs; it does not download packages or solve dependency versions.

```cmake
pipelang_add_model(checks
    PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}"
    ENTRY Entry.pipe
    SOURCE_LIBRARIES "${CMAKE_CURRENT_BINARY_DIR}/provisioned/libraries.json"
    NAMESPACE checks
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
```

`SOURCE_LIBRARIES` names an explicit local references file. The SDK CLI's
`project-info`, `project-compile-info` and `model-bindings` commands accept the same
file through `--source-libraries FILE`; legacy `MODEL`/`SOURCE_SET` inputs and view
bindings do not acquire this option. Existing projects omit it and retain their
single-root behavior and receipts.

```json
{
  "protocol": "pipelang.source-library-references.v1",
  "libraries": [
    {
      "root": "/absolute/provisioned/contracts",
      "descriptor": "library.json",
      "sha256": "<exact lowercase SHA256 of library.json bytes>"
    }
  ]
}
```

Each root must already exist as an absolute normalized nonsymbolic directory.
`descriptor` is a portable path relative to that root. The exact descriptor hash
is the admission authority, including its version identity and complete source
inventory. The provisioner creates the references file for its own filesystem;
absolute targets are not reusable library source configuration.

```json
{
  "protocol": "pipelang.source-library.v1",
  "name": "Example.Contracts",
  "version": "0.1.0",
  "source_root": "src",
  "sources": [
    {
      "path": "Contract.pipe",
      "sha256": "<exact lowercase SHA256 of Contract.pipe bytes>"
    }
  ]
}
```

A library name is a bounded ASCII identity using letters, digits, dots, underscores
or hyphens (excluding `.` and `..`). `version` is a bounded printable identity;
there is no range or version precedence interpretation. `source_root` is relative
to the library root. Inventory paths are relative to that source root, strictly
sorted, unique even after ASCII case folding, and contain only `.pipe` files.
Every discovered `.pipe` file must occur exactly once in the inventory, including
unselected modules. Every file's bytes must match its pin. Unknown JSON fields,
duplicate names (also after ASCII case folding), duplicate inputs, overlapping
source roots, traversal, symlinks/reparse entries and checksum drift fail admission.
Existing project path and aggregate resource budgets apply across all roots;
metadata also consumes indexed-byte capacity. Descriptor count is bounded by
`indexed_files`; source discovery uses shared indexed-file/directory-entry caps.

Module declarations and imports remain ordinary PipeLang. A module's explicit
parts may span files inside one owning source root. A module identity contributed
by two roots is a collision even when both declarations are explicit; library
order never shadows consumer or other library modules. Libraries are processed
in stable name order, files in stable relative-path order, and the compiler still
receives only the selected closed graph. It gains no filesystem or package access.
In library-enabled projects diagnostics and closure inventories use
`project/<path>` and `libraries/<name>/<path>` origins without rewriting source.

Receipts retain the references-file hash and each library's name, version,
provisioned root, source root and descriptor hash alongside the complete indexed
source hashes. References, descriptors and sources are build dependencies.
CMake watches each source directory for membership changes; newly introduced or
removed library sources fail their existing inventory pins until the host
explicitly provisions an updated descriptor/reference. Revalidation before
publication rescans every root and compares the complete receipt. Generated output
must remain outside all admitted source roots.

## Qualification

`pipelang-sdk-tool-tests` includes project index/refusal/drift contracts, implicit entry/import naming, explicit-name precedence,
implicit/explicit collisions, invalid-stem diagnostics, and an
installed CMake consumer using source-qualified roots, object references, source
interfaces, host interfaces, enums and generic record aliases. Its incremental
checks cover file addition, removal, content edits and import edits.
`ProjectModuleTests.cpp` adds same-module file grouping, per-file lexical imports,
union closure, deterministic input order, duplicate declarations with both origins,
checker/normalizer offsets, unchanged logical/byte limits and 4,096-fragment
admission. Its installed consumer preserves source-qualified record names and
checks incremental addition/removal of a selected module part without sleeps. The default
build and relocated-package consumer paths run this test through their existing
contained SDK test executable. The default test workflow also generates the same project consumer through the
installed SDK and runs its `project` fixture in the existing normal and ASan/UBSan
compile/run lane. `PIPELANG_NATIVE_FIXTURE_SELECTION=project` selects this fixture;
its dual root/object facets, source-interface/derived dispatch, host services,
private generic helpers, constants, owned snapshots, rollback and async cancellation
use the same source fixture as the installed CMake tests. Existing containment and platform-specific
sanitizer policies remain unchanged. Full compiler, default regression, sanitizer,
package and relocation qualification must pass before consumers switch SDK pins.
No native-platform or hosted/publication qualification follows from source staging.


Project generation publishes `Types.hpp` alongside `Model.hpp`. Both artifacts
come from the same checked declaration graph and generation transaction. The
light header contains enums, records, host interfaces, object tags and their
value/reference aliases, with the same validation/ownership helpers. It contains
no native model bodies or ordinary object execution/state implementations.
`Model.hpp` includes `Types.hpp`, adds all execution bodies and then native-root
aliases. Include `Types.hpp` for callback/data contracts and `Model.hpp` where
constructing or invoking a native owner. No public method, source identity or
metadata is pruned. SDK receipts and CMake regeneration own both artifacts;
legacy protocols retain their existing single-header output unchanged.


## Explicit compiled-project prototype

`pipelang_add_model(... PROJECT_ROOT ... ENTRY ... COMPILATION COMPILED)` selects
compiler-owned separate compilation. The default remains `HEADER_ONLY`; existing
MODEL, SOURCE_SET and view profiles do not acquire compiled output. This is a
prototype for measured build-cost qualification, not a claim of improved compile,
link or runtime performance. Do not mix the two modes for the same generated
namespace in one program or reuse a header-only output directory for compiled
publication; use a fresh build/output directory when selecting the new mode.

Both modes use the same checked and normalized program. `Types.hpp` and model
metadata are identical. In compiled mode, `Model.hpp` keeps complete native class
layouts, public constructors/methods, State, Reference and constants, but declares
method implementations. Compiler-owned `Objects.hpp` contains private object
state and dispatch declarations. Deterministically named `Implementation_N.cpp`
files contain native-owner or ordinary/interface implementation bodies; no source
algorithm is reconstructed by CMake or by a consumer. The SDK creates an internal
static library and links it through the existing INTERFACE facade. There is no
new model allocation, separate state owner, per-call task scope or public PImpl.
The native destructor executes the same member destruction order out of line.

All public methods, constructors, source-interface and virtual dispatch routes
remain available. The compiler's existing closed generic specializations are
retained. Compiled ordinary-object functions take the same concrete ObjectExecution
that the header-only templates constrain; they do not erase execution types or
alter runtime representation. Transaction preparation/commit, evaluation steps,
async suspension and scope ownership, host invocation order, cancellation, errors
and binary32 rounding stay in the existing emitter/runtime. Private and public
Source bodies pass through the same statement/expression lowering, including
nullable conversions.

The new low-level `pipelang.native-model-project.v2` transport explicitly requires
`emit: "plan"` or `emit: "sources"`. Both run full normal semantic admission. The
implementation section identifies only bounded compiler-owned plain filenames,
artifact kinds, and (for sources) their contents. At most 256 implementation units
are admitted. Initial ownership follows the
existing 128 declarations plus at most one object facet per marked root; any
additional function groups must fit the same hard unit cap or compilation fails.
Existing source/token/expansion limits and the 4 MiB encoded response bound
remain unchanged. This protocol is an internal compiler transport; it is not an
authored source or type inventory.

Implementation definitions are emitted at checked AST boundaries and grouped
within their source owner, with preferred limits of 64 KiB of emitted definition
bytes or 16 complete definitions per unit. A larger individual function occupies
its own unit intact; this preference is not a new Source acceptance limit or a
promise that an arbitrary function fits a native compiler's memory limit. State
operations, constructor wrappers/initializers, dispatch functions and native
members retain their complete bodies. The hard 256-unit and existing response
bounds remain unchanged. No C++ text parser or consumer source list selects the
partition. A body edit crossing a grouping boundary can change the checked plan;
the existing configure-plan and transactional stale-file publication rules apply.

The pinned SDK `project-compile-info` command derives and validates the implementation
plan. CMake consumes that plan at configure time and passes its identity to
`model-bindings --compilation compiled --plan-sha256 ...`. Generation verifies the
same plan and the existing source/tool drift conditions before publishing. The
v4 generated receipt owns every implementation. A changed plan requires CMake
reconfiguration; source additions/removals/import changes already trigger it.
Only receipted stale files may be removed, edited/unowned outputs refuse publication,
and missing owned artifacts may be regenerated. Transactional replacement retains
an unchanged artifact's timestamp so a body-only change need not invalidate every
public-header consumer. The whole source graph is still checked; this is not an
incremental semantic compiler or a promise of stable ordinals after declaration
changes.

The internal target is exposed by the facade's `PIPELANG_IMPLEMENTATION_TARGET`
property for inspection. The implementation consumes the facade's INTERFACE
compile options, definitions, features and include directories at generation time,
including settings added after `pipelang_add_model` and transitive interface policy
targets. Set a shared producer policy on the model facade; final consumers receive
the same interface requirements, including link options. The SDK does not create
a reverse library dependency or copy properties at configure time.

The explicitly selected compiled mode precompiles its private `Objects.hpp` once
per implementation target using that same effective producer policy. All body
partitions, optimization, floating-point, sanitizer and resource-budget settings
remain intact. This cache stays in the build tree and is not exported to archive
owners or their consumers. CMake's standard `CMAKE_DISABLE_PRECOMPILE_HEADERS`
setting can disable it; the public header-only default is unchanged.

`BATCH_IMPLEMENTATIONS` explicitly selects paired native compilation for a compiled
model. It keeps every generated source partition, receipt and plan identity, while
compiling at most two generated partitions together. It does not batch application
or native-owner sources. Actual native translation units and static archive members
can therefore span two owners, exposing additional optimization opportunities and
potentially pulling both owners' dependencies when either archive member is linked.
The default remains partitioned compilation. An explicit `CMAKE_UNITY_BUILD=OFF`
or conflicting global batch size is refused instead of overridden; the implementation
target's standard `UNITY_BUILD` property remains available for a later opt-out.
Compiler policies and physical resource limits still apply to each actual build.

Generator expressions evaluate in each actual compiling target's context. A
policy intentionally conditioned on properties unique to an executable cannot
imply the same producer settings. Likewise, PRIVATE or per-source options on a
consumer do not flow backward to a compiled dependency; different consumers may
request incompatible settings. When switching to compiled mode, move required
model code-generation policy to the model facade or an interface policy target
linked there. Directory/toolchain flags continue to apply normally. The generated
target uses C++20, PIC and the installed ModelRuntime, without replacing
optimization, floating-point or sanitizer flags. No prebuilt project binary is
inserted into the PipeLang SDK.

## Explicit native C++ compile-resource profile

Native compiler GC policy is separate from PipeLang source admission and runtime
limits. The installed SDK keeps the host compiler's defaults unless an operator
selects `gnu-conservative-gc`:

```sh
cmake -S /project -B /build \
    -DCMAKE_PREFIX_PATH=/qualified/sdk \
    -DPipeLangModel_CXX_RESOURCE_PROFILE=gnu-conservative-gc
```

`PipeLangModel_CXX_RESOURCE_PROFILE` is an explicit normal or cache CMake variable.
When defined, it overrides `PIPELANG_CXX_RESOURCE_PROFILE` in the environment.
When neither is defined, the selection is `system`. Both selectors accept only
`system` and `gnu-conservative-gc`; an explicit empty or unknown value is an error.
Environment selections are not copied into the CMake cache: reconfiguration reads
the current environment unless a CMake value overrides it. Use an explicit cache
value when automatic reconfiguration must retain the choice independently of the
build process's environment.

The GNU profile requires CMake's actual `GNU` C++ compiler identity. Clang and
other compiler profiles refuse it, including compilers offering GNU-compatible
options. The selected artifact is
`share/pipelang-model/compiler-profiles/gnu-conservative-gc.rsp`, containing only:

```text
--param=ggc-min-expand=10
--param=ggc-min-heapsize=4096
```

The SDK manifest covers this file and the package's compile-resource helper.
`PipeLangModel_CXX_RESOURCE_PROFILE_DIRECTORY` identifies the installed profile
directory after relocation. CMake copies the exact response bytes into a
content-addressed build directory, passes one quoted `@response` argument to each
C++ compilation, and watches the canonical file for reconfiguration. Changing its
content changes compile commands even when generated C++ is unchanged. Model
generation also depends on the canonical artifact, so generation rechecks the
selected SDK's manifest. Paths containing spaces are supported.

The profile applies to `pipelang_add_model` and `pipelang_add_model_bindings`
facades. Header consumers inherit it; compiled-project implementations mirror it
through their existing facade policy, and final consumers receive the same choice.
When explicitly selected while building the SDK itself, the same response file
applies to that build's native C++ targets. It does not become an installed
`PipeLang::ModelRuntime` default or an installed native archive's usage requirement.
Raw C++ invocations can use the installed response file as one quoted argument,
after checking the actual GNU compiler and verifying the selected SDK.

These parameters trade more frequent GCC garbage collection for a lower retained
compiler heap; they do not disable instrumentation, change optimization, or raise
resource caps. One original async-finally fixture under its unchanged 1 GiB unit
limit and `-O1 -g1 -fsanitize=address,undefined` compiled in 46.599 seconds with a
650,801,152-byte aggregate peak, then passed all 56 runtime assertions with leak
detection. The corresponding untuned compile exceeded its 900-second limit under
memory pressure. This establishes a bounded profile-specific result, not a general
speed or memory guarantee, and the untuned limitation remains recorded. The staged
SDK integration and cross-platform policy contracts require their own runs.

## Installing a private compiled implementation

A package exposing an opaque native STATIC component can install its private
compiled model dependency through the SDK-owned helper:

```cmake
target_link_libraries(native_component PRIVATE "$<BUILD_INTERFACE:project_model>")
pipelang_install_model_implementation(project_model
    OWNER native_component EXPORT NativePackageTargets
    ARCHIVE_DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(TARGETS native_component EXPORT NativePackageTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(EXPORT NativePackageTargets NAMESPACE NativePackage::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/NativePackage")
```

The helper installs the existing implementation archive in the owner's export
set and adds its install-only private link dependency. It does not recompile,
merge or duplicate generated definitions. Multiple native owners may share the
same exported archive; repeated identical registration is idempotent, while a
different export set or destination is refused. HEADER_ONLY and record-only
projects need no archive. This API requires an existing, non-imported STATIC
owner; exported Source APIs and shared/plugin ownership are separate contracts.

Only native public headers selected by the package are installed. Generated
Model.hpp, Objects.hpp, Types.hpp, constants, producer include paths and Source
files are not installed by this helper. The native package config must call
`find_dependency(Threads)` before including its export, and retain its own host
library dependencies. Installed consumers need neither the compiler executable
nor a PipeLang CMake target merely to link this private implementation archive.
The archive is an internal imported target in the native package namespace,
not a newly advertised Source API.

Producer compile options/definitions/features remain attached to the model
facade and are evaluated normally for every implementation unit. They are not
copied into unrelated installed native consumers. Final-link requirements such
as sanitizer runtime flags remain the native package/consumer's explicit policy:
a sanitizer producer must be paired with a matching sanitizer consumer. The
helper does not infer flags or convert a prebuilt normal archive to an
instrumented one. The owning fixture checks normal and fully instrumented
producer/consumer pairs, arbitrary direct/transitive producer policy, exact
archive closure, and relocation without private headers or producer paths in
CMake usage requirements.

There is an explicit tradeoff: header-only compilation can leave unused inline or
template bodies uninstantiated, whereas compiled output builds all admitted public
routes. Lower repeated consumer work may coexist with a larger implementation
archive or more total frontend work. Initial units group one native root or source
class/interface; the prototype does not yet partition a single very large root.
Measure the complete archive/link and unchanged runtime workload before choosing
this mode for a production dependency.

The owning test workflow accepts `PIPELANG_NATIVE_FIXTURE_SELECTION=compiled-project`.
It checks deterministic plans/publication, stale removal, unchanged-header
incrementality, two C++ translation units sharing a model, and the existing project
facet corpus through a compiled archive. Normal and ASan/UBSan builds instrument
all generated implementation units and consumers. The baseline-comparison tool
mode checks complete default responses against the prior qualified compiler.
These routes are staged contracts until their actual contained runs pass; Linux
results do not establish Windows/macOS or hosted qualification.


## Opt-in linked native runtime

Header-only SDK runtime bodies remain the default. A Linux CMake consumer may
explicitly compile the SDK's canonical runtime bodies once into a STATIC archive
and share that archive across compiled models in one executable link domain:

```cmake
add_library(runtime_policy INTERFACE)
target_compile_options(runtime_policy INTERFACE -O2 -Wall -Wextra -Werror)
pipelang_add_linked_runtime(native_runtime POLICY_TARGET runtime_policy)
pipelang_add_model(project_model
    PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" ENTRY Entry.pipe
    NAMESPACE example_model
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMPILATION COMPILED RUNTIME_TARGET native_runtime)
```

Finalize the explicit INTERFACE policy graph before declaring the runtime. The
SDK requires an immutable, supported static policy graph, applies the same policy
to runtime and generated implementation units, and rejects registered conflicts
in ownership, profile or linkage. Dynamic policy dependency expressions are
refused. This does not universally detect late manual linking or raw-object
mixing. Public layouts, Result representation and template contracts are unchanged.
Do not mix header-only and linked runtime bodies within the executable domain.

The runtime sources are installed under
`share/pipelang-model/native-runtime`. SDK manifest checks and CMake configure
dependencies cover the runtime sources, headers and policy artifacts. Runtime
compilation is a separate input to the consumer's complete build/link budget;
measure it together with generated models, final linking and execution.

`pipelang_install_model_implementation` supports an opaque native STATIC owner
with private compiled models and their single shared runtime archive. The native
package retains its public headers and host dependencies; it exports the archive
closure without private generated headers, SDK lookup or producer policy paths.
Match normal and sanitizer producer/consumer policy explicitly. The Linux fixture
qualifies two models sharing one runtime, relocated consumer execution and ten
specific refusal cases, in normal and full `-O1 -g` ASan/UBSan/LSan builds with
leak detection and the explicit GNU GC profile. This is one executable STATIC
link-domain evidence; it makes no DSO, shared-library, LTO or other-platform claim.

The Linux native test workflow accepts
`PIPELANG_NATIVE_RUNTIME_LINKAGE=linked`; unset or `header-only` retains the
existing default. Normal generated-consumer builds retain `-O2`; sanitizer
builds retain `-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer`.
The installed-owner policy fixture independently uses full-debug `-O1 -g` in
both modes. Workflow runtime archives are built with matching normal/sanitizer
policy. Standalone fast-math stays header-only. Explicit linked selection is
refused by Windows/macOS workflow routes. No installed default or SDK pin changes.

The historical `-g1` profile result above remains historical. The original
header-only full-`-g` project gate remains unqualified, including with the GNU
profile. The opt-in linked-runtime project gate passed the unchanged 58 assertions
with full debug, ASan/UBSan/LSan and `detect_leaks=1`, including both runtime
compilations, archive/link and execution in one 900-second contained workload.
The selected source preserves the baseline compiler/generator; this qualification
does not depend on the parked compiler IR experiments. The exact final SDK also passed the complete declared Linux regression with
all subset selectors unset and explicit linked mode, plus separate normal and
full producer/consumer sanitizer runs of the first Dockpipe local-host product
journey. These results remain local and profile-specific.
