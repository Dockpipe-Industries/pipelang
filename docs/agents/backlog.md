# Backlog and migration direction

The ecosystem direction is a gradual C++ rewrite over time across Dockpipe,
PipeLang and the package ecosystem. This is incremental work, not a claim that
the current Go implementation has been replaced.

Before moving any boundary to C++, define its public contracts and fixtures,
implement one coherent responsibility, prove source and generated artifact
compatibility, measure relevant resource costs and preserve the previous
implementation until equivalent consumer coverage passes. Keep native hosts and
language semantics separate; choose stable SDK interfaces for consumers.

Follow-up work includes publishing versioned Go SDK and CLI releases, qualifying
standalone distribution channels, running the complete contained compiler suite
in the independent repository, and measuring independent build times against
the extraction baseline. These remain separate from the source extraction.

- Qualify the standalone release archives on supported native platforms, then add Homebrew and OS package registry delivery using the same immutable artifacts. Registry publication and signing credentials require explicit operator setup.
