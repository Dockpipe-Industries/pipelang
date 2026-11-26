#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

compiler=$1
llc=$2
debugger=$3
proof=$4
assets=$(cd "$(dirname "$0")/.." && pwd -P)
project="$proof/native-project"
library="$project/sdk"
llvm_bin=$(dirname "$llc")

cp -R "$assets/project" "$project"
sdk_source="$library/src/SelectionSdk.pipe"
sdk_hash=$(sha256sum "$sdk_source" | cut -d ' ' -f 1)
/usr/bin/jq -n --arg digest "$sdk_hash" '{
    protocol: "pipelang.source-library.v1", name: "NativeProof.SDK",
    version: "0.1.0", source_root: "src",
    sources: [{path: "SelectionSdk.pipe", sha256: $digest}]
}' > "$library/library.json"
descriptor_hash=$(sha256sum "$library/library.json" | cut -d ' ' -f 1)
/usr/bin/jq -n --arg root "$library" --arg digest "$descriptor_hash" '{
    protocol: "pipelang.source-library-references.v1",
    libraries: [{root: $root, descriptor: "library.json", sha256: $digest}]
}' > "$project/references.json"

repeat="$proof/native-project-repeat"
cp -R "$project" "$repeat"
/usr/bin/jq --arg root "$repeat/sdk" '.libraries[0].root = $root' \
    "$project/references.json" > "$repeat/references.json"

args=(--project-root "$project" --project-entry Entry.pipe --entry Select.Selection.Select --llc "$llc")
references=(--source-libraries "$project/references.json")
"$compiler" "${args[@]}" "${references[@]}" --output "$project/select.o"
/usr/bin/ld -o "$project/select" "$proof/entry.o" "$project/select.o"
"$project/select"
"$compiler" "${args[@]}" "${references[@]}" --profile release --output "$project/release.o"
/usr/bin/ld -o "$project/select-release" "$proof/entry.o" "$project/release.o"
"$project/select-release"

"$debugger" --batch --nx --quiet \
    -ex "directory $project/src:$library/src" -ex 'set disable-randomization off' \
    -ex 'break SelectionSdk.pipe:5' -ex run -ex 'print input' -ex bt -ex 'disable 1' -ex continue \
    "$project/select" > "$project/debug.log" 2>&1
grep -E 'Breakpoint 1, .*SelectionSdk\.Read.*SelectionSdk\.pipe:5' "$project/debug.log"
grep -E 'SelectionHelper\.Read.*SelectionHelper\.pipe:6' "$project/debug.log"
grep -E 'Select\.Selection\.Select.*Selection\.pipe:6' "$project/debug.log"
grep -F 'exited normally' "$project/debug.log"

for target in linux-arm64 windows-x64 windows-arm64 macos-x64 macos-arm64; do
    if [[ "$target" == windows-* ]]; then
        "$compiler" "${args[@]}" "${references[@]}" --target "$target" --output "$project/$target.obj"
        "$llvm_bin/llvm-readobj" --codeview "$project/$target.obj" > "$project/$target.debug"
    else
        "$compiler" "${args[@]}" "${references[@]}" --target "$target" --output "$project/$target.o"
        "$llvm_bin/llvm-dwarfdump" --debug-info "$project/$target.o" > "$project/$target.debug"
    fi
    grep -F 'SelectionSdk.pipe' "$project/$target.debug" >/dev/null
    grep -F 'SelectionHelper.pipe' "$project/$target.debug" >/dev/null
    grep -F 'Selection.pipe' "$project/$target.debug" >/dev/null
done

for target in linux-x64 windows-x64; do
    extension=.o
    if [[ "$target" == windows-x64 ]]; then
        extension=.obj
    fi
    (cd "$repeat" && "$compiler" --project-root "$repeat" --project-entry Entry.pipe \
        --entry Select.Selection.Select --source-libraries "$repeat/references.json" \
        --llc "$llc" --target "$target" --output "$repeat/$target$extension")
done
cmp "$project/select.o" "$repeat/linux-x64.o"
cmp "$project/windows-x64.obj" "$repeat/windows-x64.obj"

cat > "$repeat/drifting-llc" <<'SH'
#!/bin/bash
set -euo pipefail
printf '\n' >> "$PIPELANG_TEST_DRIFT_SOURCE"
exec "$PIPELANG_TEST_REAL_LLC" "$@"
SH
chmod 0700 "$repeat/drifting-llc"
status=0
PIPELANG_TEST_DRIFT_SOURCE="$repeat/src/Selection.pipe" PIPELANG_TEST_REAL_LLC="$llc" \
    "$compiler" --project-root "$repeat" --project-entry Entry.pipe \
    --entry Select.Selection.Select --source-libraries "$repeat/references.json" \
    --llc "$repeat/drifting-llc" --output "$repeat/linux-x64.o" > "$repeat/drift.log" 2>&1 || status=$?
test "$status" -eq 1
grep -F 'project index or source inputs changed during compilation' "$repeat/drift.log"
cmp "$project/select.o" "$repeat/linux-x64.o"

refuse() {
    local expected=$1
    local status=0
    shift
    "$compiler" "${args[@]}" "$@" --output "$project/rejected.o" > "$project/refused.log" 2>&1 || status=$?
    test "$status" -eq 1
    grep -F "$expected" "$project/refused.log"
    test ! -e "$project/rejected.o"
}
refuse 'missing imported project module: NativeProof.Sdk'
printf '\n' >> "$sdk_source"
refuse 'source library source SHA256 mismatch' "${references[@]}"

cat > "$project/src/SelectionHelper.pipe" <<'PIPE'
module NativeProof.Helper;

public Class SelectionHelper {
    public static pure int Read(int input) {
        return input + 1;
    }
}
PIPE
refuse 'SelectionHelper.pipe:5: native scalar backend supports comparison operators only'

cat > "$project/src/SelectionHelper.pipe" <<'PIPE'
module NativeProof.Helper;

public Class SelectionHelper {
    public static pure int Read(int input) {
        return SelectionHelper.Read(input);
    }
}
PIPE
refuse 'SelectionHelper.pipe:5: native direct calls cannot be recursive'

composition="$proof/composition-project"
mkdir -p "$composition/src"
cat > "$composition/src/Entry.pipe" <<'PIPE'
module CompositionDiagnostic;

public Interface Clock {
    pure int Now();
}

public Class Service {
    private Clock clock;

    public Service(Clock value) {
        clock = value;
    }

    public pure int Now() => clock.Now();
}

public composition Startup {
    singleton input Clock;
    singleton Service = Service;
    export Service;
}

public Class App {
    public static pure int Run() => 42;
}
PIPE
composition_args=(--project-root "$composition" --project-entry Entry.pipe --entry CompositionDiagnostic.App.Run --llc "$llc")
"$compiler" "${composition_args[@]}" --output "$composition/accepted.o"
cp "$composition/accepted.o" "$composition/last-accepted.o"
sed '/    singleton input Clock;/d' "$composition/src/Entry.pipe" > "$composition/invalid.pipe"
mv "$composition/invalid.pipe" "$composition/src/Entry.pipe"
status=0
"$compiler" "${composition_args[@]}" --output "$composition/accepted.o" > "$composition/refused.log" 2>&1 || status=$?
test "$status" -eq 1
grep -Fx "Entry.pipe:10: missing composition binding for constructor dependency 'CompositionDiagnostic.Clock' required by service 'CompositionDiagnostic.Service'" "$composition/refused.log"
cmp "$composition/last-accepted.o" "$composition/accepted.o"
