#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

compiler=$1
proof=$2
llc=$3
assets=$(cd "$(dirname "$0")/.." && pwd -P)

refused() {
    local label=$1
    local expected=$2
    shift 2
    local status=0
    "$compiler" "$@" > "$proof/$label.stdout" 2> "$proof/$label.stderr" || status=$?
    test "$status" -eq 1
    grep -F -- "$expected" "$proof/$label.stderr"
    test ! -e "$proof/invalid.o"
    cmp "$proof/select.o" "$proof/repeat/select.o"
}

refused type 'expected int' --source "$proof/Invalid.pipe" --entry Invalid.Invalid.Select \
    --llc "$llc" --output "$proof/invalid.o"
refused lowering 'supports comparison operators only' --source "$proof/Unsupported.pipe" \
    --entry Unsupported.Unsupported.Select --llc "$llc" --output "$proof/invalid.o"
refused emission 'LLVM object emission failed' --source "$proof/Select.pipe" \
    --entry Select.Selection.Select --llc /usr/bin/false --output "$proof/select.o"
refused target 'unsupported native target' --source "$proof/Select.pipe" \
    --entry Select.Selection.Select --llc "$llc" --target linux-riscv64 --output "$proof/invalid.o"
refused extension 'must use .obj' --source "$proof/Select.pipe" \
    --entry Select.Selection.Select --llc "$llc" --target windows-x64 --output "$proof/invalid.o"
invalid_utf8=$(printf '\377')
LC_ALL=C sed "s/source cancelled/$invalid_utf8/" "$assets/ResultOutcomes.pipe" > "$proof/InvalidResultText.pipe"
refused result-text 'string contains invalid UTF-8' --source "$proof/InvalidResultText.pipe" \
    --entry NativeProof.ResultOutcomes.App.Run --runtime --llc "$llc" --output "$proof/invalid.o"

cat > "$proof/EscapedReceiver.pipe" <<'PIPE'
module NativeProof.EscapedReceiver;

public Class Score {
    public pure int Value() => 42;
}

public model Class App {
    public App() {}

    public async Task<Result<Score,string>> Run(int input) {
        var gate = SemaphoreSlim.Create(0, 1);
        var opened = await gate.WaitAsync(1);
        return Result<Score,string>.Success(new Score());
    }
}
PIPE
refused receiver-result 'invocation-owned references cannot escape their invocation' \
    --source "$proof/EscapedReceiver.pipe" --entry NativeProof.EscapedReceiver.App.Run --host-symbol proof_escape \
    --llc "$llc" --output "$proof/invalid.o"

cat > "$proof/EscapedSemaphore.pipe" <<'PIPE'
module NativeProof.EscapedSemaphore;

public model Class App {
    public App() {}

    public Result<SemaphoreSlim,string> Run() {
        return Result<SemaphoreSlim,string>.Success(SemaphoreSlim.Create(0, 1));
    }
}
PIPE
refused semaphore-result 'Result payloads must recursively contain owned values' \
    --source "$proof/EscapedSemaphore.pipe" --entry NativeProof.EscapedSemaphore.App.Run --host-symbol proof_escape \
    --llc "$llc" --output "$proof/invalid.o"

cat > "$proof/EscapedAtomic.pipe" <<'PIPE'
module NativeProof.EscapedAtomic;

public model Class App {
    public App() {}

    public Result<Atomic<int>,string> Run() {
        return Result<Atomic<int>,string>.Success(Atomic<int>.Create(0));
    }
}
PIPE
refused atomic-result 'Result payloads must recursively contain owned values' \
    --source "$proof/EscapedAtomic.pipe" --entry NativeProof.EscapedAtomic.App.Run --host-symbol proof_escape \
    --llc "$llc" --output "$proof/invalid.o"

cat > "$proof/AwaitedLock.pipe" <<'PIPE'
module NativeProof.AwaitedLock;

public model Class App {
    public App() {}

    public async Task<Result<int,string>> Run() {
        Lock gate = Lock.Create();
        var ready = SemaphoreSlim.Create(0, 1);
        lock (gate) {
            var acquired = await ready.WaitAsync(1);
        }
        return Result<int,string>.Success(42);
    }
}
PIPE
refused awaited-lock 'lock bodies cannot suspend' \
    --source "$proof/AwaitedLock.pipe" --entry NativeProof.AwaitedLock.App.Run --host-symbol proof_lock \
    --llc "$llc" --output "$proof/invalid.o"

cat > "$proof/RecordObjectReference.pipe" <<'PIPE'
module NativeProof.RecordObjectReference;

public source Interface Score {
    pure int Read();
}

public Record Holder {
    public Score score;
}

public model Class App {
    public App() {}

    public int Run() {
        return 42;
    }
}
PIPE
refused record-object-reference 'records contain owned value fields only' \
    --source "$proof/RecordObjectReference.pipe" --entry NativeProof.RecordObjectReference.App.Run \
    --host-symbol proof_invalid_record --llc "$llc" --output "$proof/invalid.o"

cat > "$proof/ConstructorStack.pipe" <<'PIPE'
module NativeProof.ConstructorStack;

public model Class Model {
    public int value;

    public Model(FixedArray<int,4096> left, FixedArray<int,4096> right,
                 FixedArray<int,4096> third, FixedArray<int,4096> fourth,
                 FixedArray<int,4096> fifth, FixedArray<int,4096> sixth,
                 FixedArray<int,4096> seventh) {
        value = left == right ? 1 : 0;
    }

    public int Read() => value;
}
PIPE
refused constructor-stack 'native synchronous call chain exceeds the stack storage limit' \
    --source "$proof/ConstructorStack.pipe" --entry NativeProof.ConstructorStack.Model.Read \
    --host-symbol proof_constructor_stack --llc "$llc" --output "$proof/invalid.o"

cat > "$proof/wrong-machine-llc" <<'SH'
#!/bin/bash
set -euo pipefail
directory=$(dirname "$0")
cat "$directory/targets/linux-arm64.o"
SH
chmod 0700 "$proof/wrong-machine-llc"
refused object 'LLVM did not produce a relocatable object for linux-x64' --source "$proof/Select.pipe" \
    --entry Select.Selection.Select --llc "$proof/wrong-machine-llc" --target linux-x64 --output "$proof/select.o"
