# Authored testing-library contracts

This project consumes the immutable installed `PipeLang.Testing` source library
through `SOURCE_LIBRARIES`. The source tests exercise generic assertions on
values, records and enums, typed parameter rows, nested runners, setup/body/cleanup
outcomes and unexpected errors. The native entrypoint validates the returned
suite and public result envelope, duplicate registration and pre-cancelled cases.
It contains no replacement test runner.

Configure with the exact installed SDK in `CMAKE_PREFIX_PATH`; the default library
root comes from `PipeLangModel_SOURCE_LIBRARY_DIRECTORY`. A private staged
library can be selected explicitly with `PIPELANG_TESTING_LIBRARY_ROOT` without
changing an installed/default SDK. Normal and sanitizer builds must use separate
fresh output directories. Enable `PIPELANG_TESTING_SANITIZERS` to instrument both
the generated implementation and native entrypoint; run the executable through
the repository's contained Dockpipe test workflow.

A successful subset establishes the authored unit/table contract only. It does
not establish async hooks, effect replay, other-platform acceptance or SDK release.
