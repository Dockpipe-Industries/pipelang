# Workflow authoring

Use Dockpipe workflows for repository build, verification and release operations.
Workflow-specific scripts belong under their workflow's `assets/scripts`; shared
scripts may live at a higher cohesive boundary only when reused.

Use `kind: host` for host commands. Use `cwd: repo` only for tools requiring the
checkout working directory. Keep generated binaries under ignored `bin/` and
workflow artifacts in the runtime artifact scope. Do not embed deployment targets
or secrets. Validate authored workflows using the installed Dockpipe CLI.
