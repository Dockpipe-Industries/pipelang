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

The native ordinary-model regression subset is explicitly selected from the
same declared test workflow with `PIPELANG_TEST_NATIVE_MODELS=1` and an absolute,
fresh `PIPELANG_NATIVE_MODEL_PROOF` outside the checkout. It builds and installs a
fresh native SDK before running protocol, semantic/lifecycle, sanitizer, view
interoperability and SDK tooling checks. The source-owned
[`tests/native-models/qualify.py`](../../../tests/native-models/README.md) also runs
independently without Go or ForgePipe. It verifies/reuses aggregate containment;
no generated compiler or fixture may execute outside the existing capped harness.
This subset does not replace the incomplete full compiler campaign.
