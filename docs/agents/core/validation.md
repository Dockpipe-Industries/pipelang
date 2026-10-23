# Validation

Use an installed Dockpipe with `DOCKPIPE_BIN` set to its absolute path. Run
`"$DOCKPIPE_BIN" workflow validate workflows/build/config.yml` and the equivalent
for `workflows/test/config.yml`, then run both workflows with `--workflow`.

Baseline tests cover CLI dispatch, diagnostics, invocation, materialization,
legacy source/artifact/evaluation digests and focused compiler contracts. Keep
fixture byte identities unchanged. Authored extraction fixtures are distinct
from the frozen legacy corpus.

Generated execution and memory tests fail closed without verified containment.
Use `tests/containedexec/run.py` and its documented limits for those tests. A
focused baseline pass is not full compiler conformance or performance proof.
Any Go cache override used for sandbox constraints must point outside sources.
