# Staging compiler release follow-up

The published Go module tag `v0.1.0` remains bound to source commit
`50c17bbf019e4736a9943513931c1c10eb8d1f7d`. Do not move that tag, replace its
draft assets, or change Dockpipe's existing Go module pin to obtain a newer CLI.
The Go SDK and standalone compiler executable are separate dependency inputs.

## Bootstrap prerequisite

CI now selects Go 1.26.10, and the Windows acceptance workflow invokes its
existing Python script directly. That workflow needs a newer public Dockpipe CLI
than the current 0.6.25 lock. `tooling/dockpipe-bootstrap-pending.json` records the
missing input; it is deliberately not an installable lock and the bootstrapper
does not consume it. This draft is not ready for hosted CI until the real lock is
updated.

After the reviewed core commit produces and publishes its CLI candidate, verify
the exact public release source identity and download all five platform assets.
Populate `dockpipe.config.json` and `dockpipe.lock.json` together with their real
version, release tag, official URLs, archive SHA256, archive size, executable name
and executable size. Use the executable's actual `--version`; a candidate's full
SemVer belongs in both files, even when its release tag differs. Keep the existing
checksum, size, official-origin and executable-version checks. Never use a sibling
build, invented hashes, a moving latest URL, or the pending declaration as a
fallback. Run the bootstrap tests and native workflow acceptance with those exact
bytes before removing the pending declaration.

## Fresh compiler identity

After the bootstrap follow-up is reviewed and committed, choose a new compiler
version, for example `0.1.1-staging.1`, set VERSION in a separate reviewed change,
and create the matching fresh source tag `v0.1.1-staging.1` at that exact commit.
The example is a proposed identity, not an existing public release. Do not reuse
`v0.1.0` or label newly built bytes as its old binaries. A new prerelease module
tag may exist without changing any consumer's v0.1.0 dependency.

The Standalone staging release workflow now cross-builds the three Go frontends
for five platforms and writes schema-2 source/version/toolchain metadata, exact
member inventories and SHA256SUMS. Native jobs download those exact uploaded
archives and check their manifest hash against the build job output. They verify
the runner platform, archive and member hashes, source and version, then exercise
all frontend help entrypoints plus valid/invalid diagnostics, compile output and
invocation on a fixed authored fixture in a temporary directory outside checkout.
The primary CLI's new `--version` path reports linker-injected version and source
identity; no compiler semantics change.

Native runners are Linux amd64 (`ubuntu-22.04`), Linux arm64
(`ubuntu-24.04-arm`), Windows amd64 (`windows-latest`), Darwin amd64
(`macos-15-intel`) and Darwin arm64 (`macos-15`). These are available hosted labels,
not claims that these jobs have run. Native acceptance is deliberately a bounded
baseline, not the full compiler conformance/generated-code/resource campaign.
The existing containment harness remains required for that campaign.

Publication runs only for an exact fresh staging tag push after all five native
jobs succeed. It checks the tag resolves to the source commit, validates every
native receipt and the complete archive set, and requires GitHub build provenance
attestations for every published file, bound to this repository, workflow, source
commit and source tag. It creates a draft prerelease with `latest=false`, or
resumes an existing exact draft; any inventory or byte mismatch fails without
replacement. An already published exact candidate is verified without editing it.
After publication it rechecks source and inventory and anonymously downloads each
asset with a strict size/hash bound. Partial or conflicting releases are left
intact and reported; the publisher does not delete, repair or overwrite them.
Manual dispatch builds and qualifies artifacts but cannot publish them.

The staging payload contains the five archives, source-bound manifest,
SHA256SUMS, and five native acceptance receipts. Existing stable v0.1.0 tags and
assets are outside this workflow's accepted identities. Release build output must
be fresh. Publication, source tagging and actual public CLI lock adoption have not
run in this draft. Future catalog compiler pinning must consume the fresh public
archive URL and checksum, not the Go module tag or sibling compiler sources.

Local draft validation includes the offline release tests and Linux development
archive acceptance with real compiler binaries. The local Go overlay injects an
example prerelease identity for testing only; it is not a committed release or
public acquisition proof. Foreign-platform records in that development fixture
are synthetic unit inputs, not native proof. All five hosted jobs, attestation
issuance/verification and public anonymous readback still need actual CI results
before this compiler release can be consumed.
