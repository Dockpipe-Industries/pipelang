# Validation

Use the installed DockPipe admitted by `dockpipe.config.json` and its matching
release lock, with `DOCKPIPE_BIN` set to its absolute path. Run root workflows
explicitly with `--workflow-file`:

```sh
"$DOCKPIPE_BIN" --workflow-file workflows/check/config.yml
"$DOCKPIPE_BIN" --workflow-file workflows/build/config.yml
"$DOCKPIPE_BIN" --workflow-file workflows/test/config.yml
```

The check workflow validates all declared workflows, authored Go formatting,
shell/Python syntax, toolchain locks, verified action pins and delivery contract
tests. It does not publish or start an application. Upgrade the config and locked
release artifacts together, then qualify the workflows with that exact runner.

Baseline tests cover CLI dispatch, diagnostics, invocation, materialization,
legacy source/artifact/evaluation digests and focused compiler contracts. Keep
fixture byte identities unchanged. Authored extraction fixtures are distinct
from the frozen legacy corpus.

Generated execution and memory tests fail closed without verified containment.
Use `tests/containedexec/run.py` and its documented limits for those tests. A
focused baseline pass is not full compiler conformance or performance proof.
Any Go cache override used for sandbox constraints must point outside sources.
