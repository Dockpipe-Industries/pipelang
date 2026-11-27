#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

source_path=$1
expected_path=$2
random=3735928559
mask=4294967295

sample() {
    random=$(((1664525 * random + 1013904223) & mask))
}

{
    cat <<'PIPE'
module NativeProof.UInt32Values;

public Class App {
    public static async Task<Result<int,string>> Run(int example) {
PIPE
    : >"$expected_path"
    for ((example = 0; example < 24; example++)); do
        sample
        expected=$random
        printf '        if (example == %d) {\n            uint32 word = %du;\n' "$example" "$expected"
        for ((operation = 0; operation < 12; operation++)); do
            sample
            operand=$((random >> 8))
            case $((random >> 29)) in
                0)
                    expected=$(((expected + operand) & mask))
                    printf '            word = uint32.WrappingAdd(word, %du);\n' "$operand"
                    ;;
                1)
                    expected=$(((expected - operand) & mask))
                    printf '            word = uint32.WrappingSubtract(word, %du);\n' "$operand"
                    ;;
                2)
                    operand=$((operand >> 8))
                    expected=$(((expected * operand) & mask))
                    printf '            word = uint32.WrappingMultiply(word, %du);\n' "$operand"
                    ;;
                3)
                    expected=$((expected ^ operand))
                    printf '            word = word ^ %du;\n' "$operand"
                    ;;
                4)
                    expected=$((expected | operand))
                    printf '            word = word | %du;\n' "$operand"
                    ;;
                5)
                    expected=$((expected & operand))
                    printf '            word = word & %du;\n' "$operand"
                    ;;
                6)
                    shift_count=$((operand & 31))
                    expected=$(((expected << shift_count) & mask))
                    printf '            word = word << %d;\n' "$shift_count"
                    ;;
                7)
                    shift_count=$((operand & 31))
                    expected=$((expected >> shift_count))
                    printf '            word = word >> %d;\n' "$shift_count"
                    ;;
            esac
        done
        printf '            return Result<int,string>.Success(int.FromUInt32(word));\n        }\n'
        printf '%d\n' "$expected" >>"$expected_path"
    done
    cat <<'PIPE'
        return Result<int,string>.Failure("unknown generated example");
    }
}

public model Class ReferenceApp {
    public ReferenceApp() {}

    public async Task<Result<int,string>> Run(int example) {
        var result = await App.Run(example);
        return result;
    }
}
PIPE
} >"$source_path"
