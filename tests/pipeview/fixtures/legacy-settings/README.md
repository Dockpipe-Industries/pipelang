# Frozen legacy settings declaration

These are the exact former example bytes qualified before ordinary-model
migration. Source-v2/state-v1 Go/reference, XML conversion and export tests use
this fixture explicitly. Current application source lives in examples/settings;
it is compiled by the separately selected native model SDK.

Preserve these bytes and the existing behavioral assertions. This fixture does
not authorize another Go implementation of the new model/async semantics.

SettingsView.pipe SHA256: 0d1e309a68b8439a9cb8fef61b0cc15ee86ad31d54796d5816b3ee021ba7677b
SettingsView.xml SHA256: 4765dafa5a0b2a0c064a1ab5599edb5f81caafaf57a23906d43f817908d26773
