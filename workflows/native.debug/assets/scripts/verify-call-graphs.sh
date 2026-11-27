#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

sdk=$1
llc=$2
proof=$3
scripts=$(cd "$(dirname "$0")" && pwd -P)
mkdir "$proof"

{
    printf '%s\n' 'module NativeProof.CallGraphs;' 'public Class App {'
    for ((index = 0; index < 24; ++index)); do
        printf '    private static pure int Part%d(int value) { return value + %d; }\n' "$index" "$index"
    done
    printf '%s\n' '    private static pure int Burst0(int value) { return value + 1; }'
    for ((index = 1; index <= 15; ++index)); do
        child=$((index - 1))
        printf '    private static pure int Burst%d(int value) { return Burst%d(Burst%d(value)); }\n' \
            "$index" "$child" "$child"
    done
    printf '%s\n' '    public static async Task<Result<int,string>> Run(int input) {' \
        '        if (input < 0)' \
        '            return Result<int,string>.Success(Burst15(0));' \
        '        int total = 0;'
    for ((index = 0; index < 24; ++index)); do
        printf '        total = total + Part%d(input);\n' "$index"
    done
    printf '%s\n' '        return Result<int,string>.Success(Burst9(total));' '    }' '}'
} > "$proof/CallGraphs.pipe"

for profile in debug release; do
    app="$proof/$profile"
    /bin/bash "$scripts/build-runtime-app.sh" "$sdk" "$llc" "$proof/CallGraphs.pipe" \
        NativeProof.CallGraphs.App.Run "$app" "$profile"
    [[ $(env -i "$app" 1) == 812 ]]
    status=0
    result=$(env -i "$app" -1 2>&1) || status=$?
    [[ $status == 1 && $result == "source evaluation budget exceeded" ]]
done

{
    printf '%s\n' 'module NativeProof.DeepCalls;' 'public Class App {' \
        '    private static pure int Step0(int value) { return value; }'
    for ((index = 1; index <= 32; ++index)); do
        printf '    private static pure int Step%d(int value) { return Step%d(value); }\n' "$index" "$((index - 1))"
    done
    printf '%s\n' '    public static async Task<Result<int,string>> Run(int value) {' \
        '        return Result<int,string>.Success(Step32(value));' '    }' '}'
} > "$proof/DeepCalls.pipe"
sed 's/Success(Step32(value))/Success(Step30(value))/' "$proof/DeepCalls.pipe" > "$proof/DepthBoundary.pipe"
/bin/bash "$scripts/build-runtime-app.sh" "$sdk" "$llc" "$proof/DepthBoundary.pipe" \
    NativeProof.DeepCalls.App.Run "$proof/depth-boundary"
[[ $(env -i "$proof/depth-boundary" 17) == 17 ]]

status=0
"$sdk/pipelang-objectc" --source "$proof/DeepCalls.pipe" --entry NativeProof.DeepCalls.App.Run \
    --runtime --llc "$llc" --output "$proof/deep.o" > "$proof/deep.log" 2>&1 || status=$?
[[ $status == 1 && ! -e "$proof/deep.o" ]]
grep -F 'native call path exceeds the source call depth limit' "$proof/deep.log"

cat > "$proof/RecursiveStack.pipe" <<'PIPE'
module NativeProof.RecursiveStack;

public model Class Session {
    public Session() {}

    private pure int Big(int remaining) {
        FixedArray<int,512> slots = FixedArray<int,512>.Create(1);
        if (remaining == 0)
            return slots.At(511);
        return Big(remaining - 1) + slots.At(remaining % 512);
    }

    public Result<int,string> Run(int input) {
        return Result<int,string>.Success(Big(input));
    }
}
PIPE
status=0
"$sdk/pipelang-objectc" --source "$proof/RecursiveStack.pipe" --entry NativeProof.RecursiveStack.Session.Run \
    --host-symbol proof_recursive_stack --llc "$llc" --output "$proof/recursive.o" \
    > "$proof/recursive.log" 2>&1 || status=$?
[[ $status == 1 && ! -e "$proof/recursive.o" ]]
grep -F 'native recursive call chain exceeds the stack storage limit' "$proof/recursive.log"

{
    printf '%s\n' 'module NativeProof.UnmeteredCalls;' 'public Class App {' \
        '    private static pure int Step0(int value) { if (value < 0) return 0; return value; }'
    for ((index = 1; index <= 15; ++index)); do
        child=$((index - 1))
        printf '    private static pure int Step%d(int value) { return Step%d(Step%d(value)); }\n' \
            "$index" "$child" "$child"
    done
    printf '%s\n' '    public static pure int Select(int value) { return Step15(value); }' '}'
} > "$proof/UnmeteredCalls.pipe"
status=0
"$sdk/pipelang-objectc" --source "$proof/UnmeteredCalls.pipe" --entry NativeProof.UnmeteredCalls.App.Select \
    --llc "$llc" --output "$proof/unmetered.o" > "$proof/unmetered.log" 2>&1 || status=$?
[[ $status == 1 && ! -e "$proof/unmetered.o" ]]
grep -F 'native unmetered calls exceed the bounded evaluation cost' "$proof/unmetered.log"

{
    printf '%s\n' 'module NativeProof.ConstructorCharge;' 'public Class Worker {' \
        '    private int value;' \
        '    public Worker(int seed) { this.value = Step5(seed); }' \
        '    public pure int Value() { return value; }' \
        '    private static pure int Step0(int value) { if (value < 0) return 0; return value; }'
    for ((index = 1; index <= 5; ++index)); do
        child=$((index - 1))
        printf '    private static pure int Step%d(int value) { return Step%d(Step%d(value)); }\n' \
            "$index" "$child" "$child"
    done
    cat <<'PIPE'
}
public Class App {
    private static async Task<Result<int,string>> Make(int seed) {
        var worker = new Worker(seed);
        return Result<int,string>.Success(worker.Value());
    }

    public static async Task<Result<int,string>> Run(int requested) {
        for (int index = 0; index < requested; index = index + 1) {
            var result = await Make(index);
            if (!result.IsSuccess)
                return result;
        }
        return Result<int,string>.Success(requested);
    }
}
PIPE
} > "$proof/ConstructorCharge.pipe"
app="$proof/constructor-charge"
/bin/bash "$scripts/build-runtime-app.sh" "$sdk" "$llc" "$proof/ConstructorCharge.pipe" \
    NativeProof.ConstructorCharge.App.Run "$app" release
[[ $(env -i "$app" 5) == 5 ]]
status=0
result=$(env -i "$app" 1000 2>&1) || status=$?
[[ $status == 1 && $result == "source evaluation budget exceeded" ]]
printf '%s\n' 'Native call graphs accepted breadth, metered repeated execution and construction, and rejected excessive depth.'
