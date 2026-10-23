# Standalone staging compiler releases

PipeLang owns the compiler, language SDKs, standalone frontends and compatibility
fixtures. Dockpipe remains an external workflow runtime. Compiler release archives
never contain Dockpipe implementation source or depend on a sibling checkout.

## Public build runtime

The producer uses the immutable public Dockpipe CLI candidate
`0.6.26-staging.cli.sha49987891f4c3`, from source
`49987891f4c3b81f5ef871b1c4cffd333e3ce274`. Its release tag is
`cli-candidate-0.6.26-staging.49987891f4c3b81f5ef871b1c4cffd333e3ce274`.
The repository config and checksum lock select its exact five platform archives.
`tooling/bootstrap_dockpipe.py` verifies archive size/hash, executable size and
native version before exposing an absolute `DOCKPIPE_BIN`.

The candidate manifest SHA256 is
`5ed8ba8cbc6995060e2deaf375b8e27370a71a0fb8251d8dfe2e23c4a877d5f1`.
Public anonymous asset verification and owning-workflow/source attestations were
checked before pin adoption. The former unresolved bootstrap marker has been
removed: it is not an acquisition input or an alternative lock.

## Source and native acceptance evidence

[Linux and Windows CI](https://github.com/Dockpipe-Industries/pipelang/actions/runs/40012151334)
and [CodeQL](https://github.com/Dockpipe-Industries/pipelang/actions/runs/40012151383)
passed for deterministic producer source
`969abd154833c3d1d2428fc22b648b437d21496d`. Both native CI jobs install the exact
public Dockpipe lock and execute the owning repository workflows.

The earlier [five-platform standalone qualification](https://github.com/Dockpipe-Industries/pipelang/actions/runs/40011399275)
passed for source `908e3e9946640278bd8f0e1868408b676a945a4c` on Linux amd64/arm64,
Windows amd64 and Darwin amd64/arm64. That manual qualification published no
compiler release. Its archive metadata exposed a reproducibility gap; the
producer now fixes archive timestamps, ownership, permissions and member ordering.
Focused regressions require identical archive bytes when input mtimes,
permissions and archive filenames differ.

These receipts establish their exact source revisions only. Every final release
source, including documentation changes, must obtain its own fresh CI and
five-platform qualification. No earlier receipt may be relabeled to a newer
commit. A staging promotion PR preserves the qualified source in staging ancestry;
the final release tag must point to that exact qualified commit.

## Release contract

`VERSION` selects `0.1.1-staging.1`. Existing `v0.1.0` tags, source and assets are
immutable and outside this lane. A compiler source tag is independent of a
consumer's pinned Go module dependency; consumers change their own pins explicitly.

The Standalone staging release workflow cross-builds the three Go frontends with
Go `1.26.10`, CGO disabled, trimpath and source/version metadata. The schema-2
manifest binds all member hashes/sizes, archive hashes/sizes, source and toolchain.
The payload contains five archives, `release-manifest.json`, `SHA256SUMS.txt`, and
five exact native acceptance receipts.

Tar/gzip archives use sorted regular members, canonical executable/document modes,
UID/GID zero, empty owner names, zero tar/gzip mtimes and no gzip filename. ZIP
archives use the fixed 1980 timestamp and canonical Unix regular-file modes.
License, README, source and version bytes retain their original layout and hashes.

Native jobs download the exact uploaded archives and verify the build manifest
hash, member inventory, source and version. In a temporary directory outside the
checkout they exercise all frontend help entrypoints, valid and invalid
diagnostics, compile output and invocation using the fixed authored fixture.
Hosted runners are `ubuntu-22.04`, `ubuntu-24.04-arm`, `windows-latest`,
`macos-15-intel` and `macos-15`. This is bounded standalone baseline acceptance;
full compiler conformance and generated-code/resource campaigns still require the
existing containment harness.

Manual dispatch on `dev` builds and qualifies without publication. Publication
requires an exact fresh `v0.1.*-staging.*` tag-push event after all five native jobs
succeed. Before creating the tag, review the final dev source's exact manifest,
checksums and receipts and confirm it is present in staging ancestry.

The publisher revalidates the complete payload and native receipts, verifies the
tag resolves to the accepted source, and requires owning GitHub workflow
attestations bound to that source and tag. It creates a draft prerelease with
`latest=false` or resumes an exact existing draft. Conflicting inventories or
bytes fail without deletion or replacement. An already published exact release
is verified without mutation. After publication, anonymous downloads must match
every asset's exact size/hash and the tag is read back again.

The source CI and manual qualification evidence above are not a claim that
`v0.1.1-staging.1` has been tagged or its binaries published. Final immutable
publication and public acquisition readback remain separate release gates.
