# PipeLang

This repository owns the PipeLang compiler, SDKs, command line tools and language
conformance fixtures. It depends on no Dockpipe Go source. Dockpipe consumes the
versioned public compiler and materializer SDKs as an external module.

Keep compiler semantics in `src/lib/pipelang`, application IR in
`src/lib/applicationir`, CLI orchestration in `src/lib/application`, and public
materialization support in `sdk/materialize`. Workflow-specific scripts belong
beside their workflow in `workflows/<name>/assets/scripts`.

Use `DOCKPIPE_BIN=/absolute/path/to/dockpipe` and invoke `workflows/check`,
`workflows/build` and `workflows/test` through explicit `--workflow-file` paths.
These run repository hygiene and bounded baseline checks. Generated compiler tests and
resource campaigns require `tests/containedexec/run.py`; never claim full-suite
coverage from the baseline workflow. Preserve frozen compatibility goldens.

Use descriptive names, ordinary formatting, clear failure propagation and bounded
resources. Do not introduce engine implementation code or sibling checkout paths.

Route tasks through `docs/agents/index.yaml`. Read focused architecture,
validation and workflow guidance as needed. `docs/agents/backlog.md` records the
gradual C++ migration direction and outstanding publication/qualification work.
