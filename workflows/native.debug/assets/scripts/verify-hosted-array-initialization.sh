#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

sdk=$1
model_compiler=$2
compiler_record=$3
llc=$4
sdk_include=$5
proof=$6
assets=$(cd "$(dirname "$0")/.." && pwd -P)
fixtures="$PWD/tests/native-models"
cxx=$(<"$compiler_record")
source="$assets/HostedArrayInitialization.pipe"

mkdir "$proof"
/usr/bin/jq -n --rawfile source "$source" '{
    protocol: "pipelang.native-model-project.v1",
    namespace: "reference",
    entry: "InitialArray",
    modules: [{name: "InitialArray", path: "HostedArrayInitialization.pipe", source: $source}]
}' | "$model_compiler" > "$proof/reference.json"
/usr/bin/jq -er 'select(.ok) | .header' "$proof/reference.json" > "$proof/InitialArray.hpp"
/usr/bin/jq -er 'select(.ok) | .types_header' "$proof/reference.json" > "$proof/Types.hpp"

/usr/bin/jq -n --rawfile source "$fixtures/Collections.pipe" '{
    protocol: "pipelang.native-model.v1", namespace: "pipelang_models", source: $source
}' | "$model_compiler" > "$proof/reference-collections.json"
/usr/bin/jq -er 'select(.ok) | .header' "$proof/reference-collections.json" > "$proof/Collections.hpp"

common=(-std=c++20 -O2 -pthread -Wall -Wextra -Wpedantic -Werror -I "$sdk_include")
"$cxx" "${common[@]}" -I "$proof" -DPIPELANG_ARRAY_CPP_REFERENCE "$assets/HostedArrayInitialization.cpp" -o "$proof/reference"
"$cxx" "${common[@]}" -I "$proof" "$fixtures/collections_source.cpp" -o "$proof/reference-collections"
env -i "$proof/reference" > "$proof/reference.output"
env -i "$proof/reference-collections"

"$cxx" "${common[@]}" -fPIE -c "$assets/HostedArrayInitialization.cpp" -o "$proof/driver.o"
"$cxx" "${common[@]}" -fPIE -c "$assets/OriginalCollectionsNative.cpp" -o "$proof/collections-driver.o"
runtime="$sdk/libpipelang_native_app_runtime.a"
for profile in debug release; do
    "$sdk/pipelang-objectc" --source "$source" --entry InitialArray.Model.Read --host-symbol proof_initial_array \
        --host-method Edit --host-method ReadBytes --host-method ReadSaved --host-snapshot \
        --profile "$profile" --llc "$llc" --output "$proof/$profile.o"
    "$cxx" -pie -pthread "$proof/driver.o" "$proof/$profile.o" "$runtime" -o "$proof/$profile"
    env -i "$proof/$profile" > "$proof/$profile.output"
    cmp "$proof/reference.output" "$proof/$profile.output"

    "$sdk/pipelang-objectc" --source "$fixtures/Collections.pipe" --entry Collections.CollectionModel.Seed \
        --host-symbol proof_collections --host-snapshot --profile "$profile" --llc "$llc" --output "$proof/collections-$profile.o" \
        --host-method ArrayCopies --host-method MappingOrder --host-method MappingIteration --host-method DictionaryOptions \
        --host-method SetOrder --host-method PrimitiveKeys --host-method SequenceOrder --host-method SequenceOperations \
        --host-method OwnedBundle --host-method MissingRollback --host-method IndexRollback --host-method DuplicateRollback \
        --host-method EmptyRollback --host-method EditFields --host-method TemporaryOptionalRead --host-method TemporaryResultRead \
        --host-method ReceiverBeforeArguments --host-method WrappedIsEmpty
    "$cxx" -pie -pthread "$proof/collections-driver.o" "$proof/collections-$profile.o" "$runtime" -o "$proof/collections-$profile"
    env -i "$proof/collections-$profile"
done
cat "$proof/reference.output"
