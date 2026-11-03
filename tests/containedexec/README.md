# Retired orchestration

The former Python orchestration was removed. Use the default native DockPipe
workflows and [validation guidance](../../docs/agents/core/validation.md).
The editor extension source remains under tooling; Marketplace publication is
not part of the compiler build or SDK package.

The Linux native probe also exercises the internal proof-estate sampler against
allocated-file byte limits, permission errors, non-traversed symlink targets,
redirected roots, disappearing paths and finite directory publication/removal
churn. ENOENT requires a full rescan, with at most eight attempts sharing the
existing million-entry visit bound; only a complete traversal can succeed and
partial observed allocation is retained conservatively. Persistent instability
fails closed. The probe additionally checks that live allocation growth past an
estate cap terminates and reaps its workload. These checks supplement the nine
established containment failure contracts without changing their receipt schema.
This Linux scanner qualification does not qualify the separate macOS sampler.
