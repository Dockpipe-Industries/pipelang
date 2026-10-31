package main

import (
	"context"
	"fmt"
	"math"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang/coreeval"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang/coreir"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang/cppbackend"
)

const helperCallsSource = `public Class Calls {
    public float Choose(bool choose, float first, float second) => choose ? first : second;
    public float Identity(float value) => value;
    public float Constant() => 0.5;
    public float Zero() => Constant();
    public bool Less(float first, float second) => first < second;
    public float Minimum(float first, float second) => Less(second, first) ? Identity(second) : Identity(first);
    public float Compose(bool choose, float first, float second) => Choose(choose, Identity(first), Identity(second));
    public float Order() => Choose(true, Identity(0.25), Identity(0.75));
    public float Lazy(bool choose) => choose ? Identity(0.25) : Identity(0.75);
    public bool Range(float value, float minimum, float maximum) => value >= minimum && value <= maximum;
    public bool Coefficients(float first, float second) => Range(first, 0.0, 10.0) && Range(second, 0.0, 10.0) && Less(second, first);
}`

func TestFloat64CallsNativeSourceAndHostValues(t *testing.T) {
	emitter, cxx, _ := float64NativeTools(t)
	var program coreir.Program
	generate := func(admitted coreir.Program) (cppbackend.Generated, error) {
		program = admitted
		return emitter.GenerateWithProfile(context.Background(), admitted, cppbackend.Float64CallsCapability)
	}
	header, metadata, err := compileWithProfileComponents([]byte(helperCallsSource), "Calls.pipe", "example.calls", "app.calls", cppbackend.Float64CallsCapability, generate, nil)
	if err != nil {
		t.Fatal(err)
	}
	if metadata.Profile != cppbackend.Float64CallsCapability || len(metadata.Functions) != 11 {
		t.Fatal("source call metadata changed")
	}
	names := make(map[string]string)
	functions := make(map[string]coreir.Function)
	for _, function := range metadata.Functions {
		names[function.Name] = function.CppSymbol
	}
	for _, function := range program.Functions {
		functions[function.Name] = function
	}
	var checks strings.Builder
	count := 0
	values := []uint64{0xfff0000000000000, 0xffefffffffffffff, 0xbff0000000000000,
		0x8010000000000000, 0x8000000000000001, 0x8000000000000000, 0, 1,
		0x0010000000000000, 0x3fefffffffffffff, 0x3ff0000000000000, 0x3ff0000000000001,
		0x7fefffffffffffff, 0x7ff0000000000000, 0x7ff8000000000001, 0xfff8000000000042}
	for _, first := range values {
		for _, second := range values {
			minimum := first
			if conditionalLess(second, first) {
				minimum = second
			}
			for _, method := range []string{"Minimum", "Compose"} {
				for _, choose := range []bool{false, true} {
					expected := minimum
					function := functions[method]
					arguments := []coreeval.Value{}
					callArguments := ""
					if method == "Compose" {
						expected = second
						if choose {
							expected = first
						}
						arguments = append(arguments, coreeval.Value{Type: function.Parameters[0].Type, Bool: choose})
						callArguments = fmt.Sprint(choose) + ", "
					}
					offset := len(arguments)
					arguments = append(arguments,
						coreeval.Value{Type: function.Parameters[offset].Type, Float: math.Float64frombits(first)},
						coreeval.Value{Type: function.Parameters[offset+1].Type, Float: math.Float64frombits(second)})
					outcome, err := coreeval.EvaluateProgram(program, function.Identity, arguments)
					if err != nil || !outcome.OK {
						t.Fatalf("existing Core call behavior: %v", err)
					}
					callArguments += fmt.Sprintf("from_bits(UINT64_C(0x%016x)), from_bits(UINT64_C(0x%016x))", first, second)
					call := names[method] + "(" + callArguments + ")"
					if conditionalNaN(expected) {
						if !math.IsNaN(outcome.Value.Float) {
							t.Fatal("Core call NaN classification differs")
						}
						fmt.Fprintf(&checks, "    if (!std::isnan(%s)) return 1;\n", call)
					} else {
						if math.Float64bits(outcome.Value.Float) != expected {
							t.Fatal("Core call selected bits differ")
						}
						fmt.Fprintf(&checks, "    if (bits(%s) != UINT64_C(0x%016x)) return 2;\n", call, expected)
					}
					count++
				}
			}
		}
	}
	fmt.Fprintf(&checks, "    if (%s() != 0.5) return 3;\n", names["Zero"])
	directory := float64ProofDirectory(t)
	writeFloat64Proof(t, filepath.Join(directory, "calls.hpp"), header)
	main := `#include "calls.hpp"
#include <cmath>
using pipelang::float64_comparison_detail::from_bits;
std::uint64_t bits(double value) {
    std::uint64_t result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}
int main() {
` + checks.String() + "    return 0;\n}\n"
	writeFloat64Proof(t, filepath.Join(directory, "calls.cpp"), []byte(main))
	binary := filepath.Join(directory, "calls")
	runFloat64Proof(t, exec.Command(cxx, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", filepath.Join(directory, "calls.cpp"), "-o", binary))
	runFloat64Proof(t, exec.Command(binary))
	// Instrument only literal reconstruction in the test header. The ordered call
	// arguments must each run once; the lazy conditional must run one arm only.
	instrumented := strings.Replace(string(header), "namespace pipelang::float64_comparison_detail {", "namespace pipelang::float64_comparison_detail {\ninline std::uint64_t reads[8];\ninline int count = 0;", 1)
	instrumented = strings.Replace(instrumented, "    double value;", "    reads[count++] = bits;\n    double value;", 1)
	writeFloat64Proof(t, filepath.Join(directory, "trace.hpp"), []byte(instrumented))
	trace := fmt.Sprintf(`#include "trace.hpp"
int main() {
    using namespace pipelang::float64_comparison_detail;
    if (%s() != 0.25 || count != 2)
        return 1;
    if (reads[0] != UINT64_C(0x3fd0000000000000) || reads[1] != UINT64_C(0x3fe8000000000000))
        return 2;
    count = 0;
    if (%s(false) != 0.75 || count != 1 || reads[0] != UINT64_C(0x3fe8000000000000))
        return 3;
    count = 0;
    if (%s(true) != 0.25 || count != 1 || reads[0] != UINT64_C(0x3fd0000000000000))
        return 4;
    return 0;
}
`, names["Order"], names["Lazy"], names["Lazy"])
	writeFloat64Proof(t, filepath.Join(directory, "trace.cpp"), []byte(trace))
	traceBinary := filepath.Join(directory, "trace")
	runFloat64Proof(t, exec.Command(cxx, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", filepath.Join(directory, "trace.cpp"), "-o", traceBinary))
	runFloat64Proof(t, exec.Command(traceBinary))
	t.Logf("%d independently oracled helper-call selected values", count)
}
