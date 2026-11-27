# TASK-022 Go-First PipeLang Backend Services And Typed Clients

## Current direction — 2026-11-08

The user explicitly selected **PipeServe as a separately owned reusable `.pipe`
HTTPS module**, consumed by DorkPipe's `.pipe` knowledge/MCP implementation. This
supersedes the Go-first production implementation and generator sequencing below
for the active slice. Python runtime implementation is rejected. The older design
is retained as historical context, not an instruction to implement a replacement
Go/C++ service or wait for desktop/Qt/application UI work.

TASK-022 remains the canonical service initiative; do not create a duplicate backlog.
PipeLang/TASK-021 owns general language/compiler/runtime capabilities and external
API interoperability. PipeServe owns `.pipe` HTTP parsing, service lifecycle and
generic identity/policy integration. Its trusted native OpenSSL binding provides
TLS accept/read/write/close and verified peer identity; it must not implement HTTP
routing, MCP dispatch or domain algorithms. DorkPipe/DORK-001 owns `.pipe` MCP,
knowledge-source grants and knowledge operations, consuming a separately installed
PipeServe package without sibling-source build coupling.

The local PipeServe candidate is `/home/jamie/source/pipeserve`; remote repository
baseline checks found no existing product implementation. Initial native MCP work
uses mandatory client certificates and explicit per-client source grants for
managed machine clients. This restricted mode does not claim OAuth client
interoperability. No inference, desktop UI, service deployment, branch publication
or release is required or authorized by this direction record.

The first status/source-discovery operation is a bounded foundation, not proof that
native Markdown ingestion, persistent indexes, search, graph/DAG or context services
are complete. Real HTTPS fixtures and installed-package consumption must qualify
this slice; broader knowledge acceptance remains DORK-001 work.

General UTF-8 operations needed by `.pipe` parsing are owned and qualified in
PipeLang, rather than hidden in a native HTTP implementation. Prepared local SDK
receipts identify the complete tested compiler/runtime input, including concurrent
unrelated changes; they do not establish a released compiler dependency.

## Historical Go-first proposal


## Goal

Define first-class backend web-service support across PipeLang and Dockpipe so one typed,
transport-neutral service contract can produce and verify:

- a standalone Go backend service;
- typed Qt/C++ client bindings for native and supported WebAssembly builds;
- OpenAPI and JSON Schema artifacts;
- a deterministic in-memory service implementation;
- contract, serialization, authorization, and real HTTP integration tests; and
- Dockpipe build, packaging, execution, deployment, and artifact-verification outputs.

Go is the required first production backend target. This aligns with Dockpipe's implementation,
portable-runtime and cross-compilation direction, preference for standalone binaries, and low
operational overhead. ASP.NET Core, Qt HTTP Server, WASI, and other ecosystems may become future
resolver targets, but they must not shape PipeLang core or the transport-neutral service model.

This is a future backlog/design initiative only. It does not authorize PipeLang syntax, parser,
semantic model, Service IR, resolver, Go generator, Qt client generator, schema generator, workflow,
runtime, package, deployment, generated-state, or toolchain changes.

## Dependencies And Ownership

The 2026-09-11 [PipeLang foundation plan](../../../concepts/pipelang-foundation.md) requires the
reviewed language-foundation milestone before production application/service generation. It includes
enums, structs/classes, polymorphism, loops, managed tasks, parallelism and synchronization. Existing
fixtures remain evidence, not proof that the language is complete. TASK-021 owns the object/memory/
task/atomic/lock/semaphore semantics; target adapters must consume them and cannot invent them.
The completed [foundation dependency plan](../../../concepts/pipelang-foundation-delivery.md)
proposes M-app after the full managed foundation and minimum compiler library, with compiler
port/bootstrap separately scheduled. Founder ratification remains pending; no app/service
implementation is authorized. Physical target certification and target delivery estimates remain
separate work, and adapters cannot weaken the selected profile or invent missing semantics.


- [TASK-021](../pipelang-reactive-application-language/overview.md) is the language prerequisite. It owns stable
  semantic IDs, types, optionals/collections/unions, contracts, effects/authority, determinism,
  replay, semantic graphs, typed HIR/Core IR, executable entrypoints, target profiles, self-hosting,
  and compatibility.
- [TASK-020](https://github.com/Dockpipe-Industries/dockpipe/blob/master/docs/agents/tasks/declarative-application-surfaces-and-target-builders/overview.md) owns application surfaces and
  consumption of generated Qt clients. It does not own service or transport semantics.
- TASK-022 owns service declarations, transport-neutral operation semantics, the independently
  versioned Service IR specialization over TASK-021's semantic/Core foundation, resolver capability
  negotiation, Go service generation, Qt client generation, schemas, service testing, packaging,
  and deployment artifacts.

The architecture remains workflow = what, runtime = where, resolver = which platform/tool, and
strategy = lifecycle wrapper. Service compilation is workflow/package intent. Go, HTTP, OpenAPI,
Qt, containers, hosts, VMs, and remote execution are resolver/runtime outputs or selections rather
than new engine primitives.

TASK-021's deterministic Go backend lowers generic executable Core IR and seeds compiler bootstrap.
TASK-022's Go service resolver is a separate specialization that consumes Service IR to add HTTP,
service lifecycle, schema, and packaging behavior. Neither owns or modifies the other's semantics.

## Current Reusable Foundation

Repository investigation must precede design or implementation. Relevant existing ownership includes:

| Foundation | Existing owner/direction |
| --- | --- |
| PipeLang v0.0.0.1 | `docs/concepts/pipelang.md`, `src/lib/pipelang/`, application PipeLang commands/tests |
| Architecture invariants | `docs/concepts/architecture-model.md` and focused agent architecture guidance |
| Capability/resolver packages | package `capability:` plus workflow/package `requires_capabilities` and existing resolver compilation |
| Workflow/runtime/resolver/strategy | existing authored YAML, compiled packages, selection, and validation paths |
| Package-owned implementation | `packages/**` YAML, resolver profiles, assets/scripts, tests, and repo-local binary rules |
| Generated artifacts | project/package artifact manifests, scoped artifact paths, hashes/provenance, and operation results |
| Secret handling | reference-only templates and resolver-owned secret injection; no plaintext generated source |
| Go HTTP evidence | existing package-local `net/http` servers/tests demonstrate conventions but are not a generic service framework |
| Remote and isolated execution | existing Dockpipe runtime/resolver selection and task-owned artifacts rather than service-specific infrastructure |

Do not introduce parallel package, capability, artifact, secret, approval, remote-execution, or
runtime abstractions when current Dockpipe contracts can carry the requirement. Current HTTP servers
are implementation evidence only; they do not define the Service IR or generator contract.
