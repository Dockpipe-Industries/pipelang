package main

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang/coreeval"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang/coreir"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang/cppbackend"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang/gobackend"
	"github.com/Dockpipe-Industries/pipelang/tests/containedexec"
)

const conditionalSource = `public Class Values {
    public float Minimum(float first, float second) => second < first ? second : first;
    public float Select(bool choose, float first, float second) => choose ? first : second;
    public float Identity(float value) => value;
    public float Literal(bool choose) => choose ? 0.1 : 0.0;
    public float Nested(bool choose, float first, float second)
        => choose ? (second < first ? second : first) : second;
    public bool Boolean(bool choose, float first, float second)
        => choose ? first < 0.0 : second < 0.0;
    public float Trace(bool choose) => choose && 0.5 < 1.0 ? 2.0 : 3.0;
}`

func compileConditionalFixture(t *testing.T, source string) (coreir.Program, cppbackend.Generated, metadata) {
	t.Helper()
	var program coreir.Program
	var generated cppbackend.Generated
	emit := func(admitted coreir.Program) (cppbackend.Generated, error) {
		program = admitted
		var err error
		generated, err = cppbackend.GenerateWithProfile(admitted, cppbackend.Float64ConditionalCapability)
		return generated, err
	}
	sourceBytes, receipt, err := compileWithProfileComponents([]byte(source), "Values.pipe", "example.values", "app.values", cppbackend.Float64ConditionalCapability, emit, nil)
	if err != nil || !bytes.Equal(sourceBytes, generated.Source) {
		t.Fatalf("conditional source pipeline: %v", err)
	}
	return program, generated, receipt
}

func TestFloat64ConditionalSourceAdmission(t *testing.T) {
	_, generated, receipt := compileConditionalFixture(t, conditionalSource)
	if receipt.Profile != cppbackend.Float64ConditionalCapability || len(receipt.Functions) != 7 || receipt.GeneratedSHA256 != digest(generated.Source) {
		t.Fatal("conditional export identity differs")
	}
	for _, source := range []string{
		`public Class X { public float Add(float x, float y) => x + y; }`,
		`public Class X { public float Negative(float x) => -x; }`,
		`public Class X { public float Local(float x) { float y = x; return y; } }`,
		`public Class X { public float Terminal(bool b, float x, float y) { if (b) { return x; } else { return y; } } }`,
		`public Class X { public float Helper(float x) => x; public float Call(float x) => Helper(x); }`,
		`public Class X { public int Integer(bool b) => b ? 1 : 2; }`,
	} {
		output, _, err := compileWithProfileComponents([]byte(source), "Refuse.pipe", "example.values", "app.values", cppbackend.Float64ConditionalCapability,
			func(program coreir.Program) (cppbackend.Generated, error) {
				return cppbackend.GenerateWithProfile(program, cppbackend.Float64ConditionalCapability)
			}, nil)
		if err == nil || len(output) != 0 {
			t.Fatalf("unsupported source admitted: %s", source)
		}
	}
}

func conditionalNaN(bits uint64) bool {
	return bits&0x7ff0000000000000 == 0x7ff0000000000000 && bits&0x000fffffffffffff != 0
}

// Order IEEE encodings independently of floating-point comparisons. Both zero
// encodings compare equal; NaNs are unordered. This also orders infinities.
func conditionalLess(left, right uint64) bool {
	if conditionalNaN(left) || conditionalNaN(right) || (left<<1 == 0 && right<<1 == 0) {
		return false
	}
	order := func(bits uint64) uint64 {
		if bits>>63 != 0 {
			return ^bits
		}
		return bits | 0x8000000000000000
	}
	return order(left) < order(right)
}

func TestFloat64ConditionalIEEEParity(t *testing.T) {
	emitter, cxx, goTool := float64NativeTools(t)
	program, reference, _ := compileConditionalFixture(t, conditionalSource)
	native, err := emitter.GenerateWithProfile(context.Background(), program, cppbackend.Float64ConditionalCapability)
	if err != nil || !bytes.Equal(reference.Source, native.Source) {
		t.Fatalf("conditional emitter header mismatch: %v", err)
	}
	goGenerated, err := gobackend.GenerateWithNames(program)
	if err != nil {
		t.Fatal(err)
	}
	functions := make(map[string]coreir.Function)
	cppNames := make(map[string]string)
	goNames := make(map[string]string)
	for _, function := range program.Functions {
		functions[function.Name] = function
		for _, binding := range reference.Functions {
			if binding.Identity.Path == function.Identity.Path {
				cppNames[function.Name] = binding.Name
			}
		}
		for _, binding := range goGenerated.Functions {
			if binding.Identity.Path == function.Identity.Path {
				goNames[function.Name] = binding.Name
			}
		}
	}
	var cppChecks, goChecks strings.Builder
	count := 0
	check := func(method string, values []uint64, choose bool, expected uint64, boolean bool) {
		function := functions[method]
		var arguments []coreeval.Value
		var cppArgs, goArgs []string
		index := 0
		for _, parameter := range function.Parameters {
			value := coreeval.Value{Type: parameter.Type}
			if parameter.Type.Kind == coreir.TypeNumeric {
				bits := values[index]
				index++
				value.Float = math.Float64frombits(bits)
				cppArgs = append(cppArgs, fmt.Sprintf("from_bits(UINT64_C(0x%016x))", bits))
				goArgs = append(goArgs, fmt.Sprintf("math.Float64frombits(0x%016x)", bits))
			} else {
				value.Bool = choose
				cppArgs = append(cppArgs, fmt.Sprint(choose))
				goArgs = append(goArgs, fmt.Sprint(choose))
			}
			arguments = append(arguments, value)
		}
		outcome, err := coreeval.EvaluateProgram(program, function.Identity, arguments)
		if err != nil || !outcome.OK {
			t.Fatalf("Core conditional failed: %v", err)
		}
		cppCall := "pipelang::" + cppNames[method] + "(" + strings.Join(cppArgs, ", ") + ")"
		goCall := goNames[method] + "(" + strings.Join(goArgs, ", ") + ")"
		if function.ReturnType.Kind == coreir.TypePrimitive {
			if outcome.Value.Bool != boolean {
				t.Fatal("Core Boolean result differs")
			}
			fmt.Fprintf(&cppChecks, "    if (%s != %t)\n        return %d;\n", cppCall, boolean, count%200+1)
			fmt.Fprintf(&goChecks, "    if %s != %t {\n        t.Fatal(\"case %d\")\n    }\n", goCall, boolean, count)
		} else if conditionalNaN(expected) {
			if !math.IsNaN(outcome.Value.Float) {
				t.Fatal("Core NaN classification differs")
			}
			fmt.Fprintf(&cppChecks, "    if (!std::isnan(%s))\n        return %d;\n", cppCall, count%200+1)
			fmt.Fprintf(&goChecks, "    if !math.IsNaN(%s) {\n        t.Fatal(\"case %d\")\n    }\n", goCall, count)
		} else {
			if math.Float64bits(outcome.Value.Float) != expected {
				t.Fatalf("Core exact selected bits differ at %s/%d", method, count)
			}
			fmt.Fprintf(&cppChecks, "    if (bits(%s) != UINT64_C(0x%016x))\n        return %d;\n", cppCall, expected, count%200+1)
			fmt.Fprintf(&goChecks, "    if math.Float64bits(%s) != 0x%016x {\n        t.Fatal(\"case %d\")\n    }\n", goCall, expected, count)
		}
		count++
	}
	values := []uint64{0xfff0000000000000, 0xffefffffffffffff, 0xbff0000000000000, 0x8010000000000000, 0x8000000000000001,
		0x8000000000000000, 0, 1, 0x0010000000000000, 0x3fefffffffffffff, 0x3ff0000000000000, 0x3ff0000000000001,
		0x7fefffffffffffff, 0x7ff0000000000000, 0x7ff8000000000001, 0xfff8000000000042}
	for _, first := range values {
		check("Identity", []uint64{first}, false, first, false)
		for _, second := range values {
			minimum := first
			if conditionalLess(second, first) {
				minimum = second
			}
			check("Minimum", []uint64{first, second}, false, minimum, false)
			for _, choose := range []bool{false, true} {
				selected, nested := second, second
				if choose {
					selected, nested = first, minimum
				}
				check("Select", []uint64{first, second}, choose, selected, false)
				check("Nested", []uint64{first, second}, choose, nested, false)
				check("Boolean", []uint64{first, second}, choose, 0, conditionalLess(selected, 0))
			}
		}
	}
	check("Literal", nil, false, 0, false)
	check("Literal", nil, true, math.Float64bits(0.1), false)
	directory := float64ProofDirectory(t)
	writeFloat64Proof(t, filepath.Join(directory, "reference.hpp"), reference.Source)
	writeFloat64Proof(t, filepath.Join(directory, "native.hpp"), native.Source)
	main := `#include HEADER
#include <cmath>
#include <cstring>
using pipelang::float64_comparison_detail::from_bits;
std::uint64_t bits(double value) {
    std::uint64_t result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}
int main() {
` + cppChecks.String() + "    return 0;\n}\n"
	writeFloat64Proof(t, filepath.Join(directory, "main.cpp"), []byte(main))
	for _, version := range []string{"reference", "native"} {
		binary := filepath.Join(directory, version)
		runFloat64Proof(t, exec.Command(cxx, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", `-DHEADER="`+version+`.hpp"`, filepath.Join(directory, "main.cpp"), "-o", binary))
		runFloat64Proof(t, exec.Command(binary))
	}
	goDirectory := filepath.Join(directory, "go")
	if err := os.MkdirAll(goDirectory, 0700); err != nil {
		t.Fatal(err)
	}
	writeFloat64Proof(t, filepath.Join(goDirectory, "generated.go"), goGenerated.Source)
	writeFloat64Proof(t, filepath.Join(goDirectory, "generated_test.go"), []byte("package pipelanggenerated\n\nimport (\n    \"math\"\n    \"testing\"\n)\n\nfunc TestSelectedValues(t *testing.T) {\n"+goChecks.String()+"}\n"))
	command := exec.Command(goTool, "test", "-p", "1", "-count=1")
	command.Dir = goDirectory
	command.Env = append(os.Environ(), "GO111MODULE=off")
	runFloat64Proof(t, command)
	for _, flag := range []string{"-ffast-math", "-ffinite-math-only"} {
		output, err := containedexec.CombinedOutput(exec.Command(cxx, "-std=c++17", flag, `-DHEADER="reference.hpp"`, "-fsyntax-only", filepath.Join(directory, "main.cpp")))
		if err == nil || !bytes.Contains(output, []byte("require strict IEEE")) {
			t.Fatalf("unsafe flag admitted: %s", flag)
		}
	}
	t.Logf("%d independently oracled selected-value cases passed all four paths", count)
}

func TestFloat64ConditionalLazyArms(t *testing.T) {
	emitter, cxx, _ := float64NativeTools(t)
	program, reference, _ := compileConditionalFixture(t, `public Class X { public float Trace(bool choose) => choose && 0.5 < 1.0 ? 2.0 : 3.0; }`)
	native, err := emitter.GenerateWithProfile(context.Background(), program, cppbackend.Float64ConditionalCapability)
	if err != nil || !bytes.Equal(reference.Source, native.Source) {
		t.Fatalf("trace headers differ: %v", err)
	}
	// Instrument only the test header: trace literal reconstruction order.
	instrumented := strings.Replace(string(reference.Source), "namespace pipelang::float64_comparison_detail {", "namespace pipelang::float64_comparison_detail {\ninline std::uint64_t reads[8];\ninline int count = 0;", 1)
	instrumented = strings.Replace(instrumented, "    double value;", "    reads[count++] = bits;\n    double value;", 1)
	main := fmt.Sprintf(`#include "trace.hpp"
int main() {
    using namespace pipelang::float64_comparison_detail;
    if (pipelang::%s(true) != 2.0 || count != 3)
        return 1;
    if (reads[0] != UINT64_C(0x3fe0000000000000) || reads[1] != UINT64_C(0x3ff0000000000000) || reads[2] != UINT64_C(0x4000000000000000))
        return 2;
    count = 0;
    if (pipelang::%s(false) != 3.0 || count != 1)
        return 3;
    if (reads[0] != UINT64_C(0x4008000000000000))
        return 4;
    return 0;
}
`, reference.Functions[0].Name, reference.Functions[0].Name)
	directory := float64ProofDirectory(t)
	writeFloat64Proof(t, filepath.Join(directory, "trace.hpp"), []byte(instrumented))
	writeFloat64Proof(t, filepath.Join(directory, "trace.cpp"), []byte(main))
	binary := filepath.Join(directory, "trace")
	runFloat64Proof(t, exec.Command(cxx, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", filepath.Join(directory, "trace.cpp"), "-o", binary))
	runFloat64Proof(t, exec.Command(binary))
}

func TestFloat64ConditionalCLIRefusesBeforeWriting(t *testing.T) {
	emitter, _, _ := float64NativeTools(t)
	frontend := os.Getenv("PIPELANG_FLOAT64_FRONTEND")
	if !filepath.IsAbs(frontend) {
		t.Skip("explicit current frontend required")
	}
	directory := float64ProofDirectory(t)
	failedEmitter := filepath.Join(directory, "failed-emitter")
	failedScript := []byte("#!/bin/sh\nprintf 'deliberate emitter failure' >&2\nexit 7\n")
	if err := os.WriteFile(failedEmitter, failedScript, 0700); err != nil {
		t.Fatal(err)
	}
	failedHash := sha256.Sum256(failedScript)
	cases := []struct {
		name    string
		source  string
		options []string
	}{
		{"unknown-profile", conditionalSource, []string{"--profile", "unknown"}},
		{"default-pilot", conditionalSource, nil},
		{"missing-hash", conditionalSource, []string{"--profile", cppbackend.Float64ConditionalCapability, "--cpp-emitter", emitter.Path}},
		{"missing-path", conditionalSource, []string{"--profile", cppbackend.Float64ConditionalCapability, "--cpp-emitter-sha256", emitter.SHA256}},
		{"wrong-hash", conditionalSource, []string{"--profile", cppbackend.Float64ConditionalCapability, "--cpp-emitter", emitter.Path, "--cpp-emitter-sha256", strings.Repeat("0", 64)}},
		{"process-failure", conditionalSource, []string{"--profile", cppbackend.Float64ConditionalCapability, "--cpp-emitter", failedEmitter, "--cpp-emitter-sha256", hex.EncodeToString(failedHash[:])}},
		{"unsupported-shape", `public Class Root {public float Add(float left,float right)=>left+right;}`, []string{"--profile", cppbackend.Float64ConditionalCapability}},
	}
	for _, test := range cases {
		t.Run(test.name, func(t *testing.T) {
			source := filepath.Join(directory, test.name+".pipe")
			writeFloat64Proof(t, source, []byte(test.source))
			for _, existing := range []bool{false, true} {
				output := filepath.Join(directory, fmt.Sprintf("%s-%t", test.name, existing))
				if existing {
					if err := os.Mkdir(output, 0700); err != nil {
						t.Fatal(err)
					}
					writeFloat64Proof(t, filepath.Join(output, "generated.hpp"), []byte("header sentinel"))
					writeFloat64Proof(t, filepath.Join(output, "bindings.json"), []byte("receipt sentinel"))
				}
				arguments := append([]string{"--source", source, "--output-dir", output}, test.options...)
				diagnostic, err := containedexec.CombinedOutput(exec.Command(frontend, arguments...))
				if err == nil || len(diagnostic) == 0 {
					t.Fatal("unsupported selection succeeded or omitted its diagnostic")
				}
				if !existing {
					if _, err := os.Stat(output); !os.IsNotExist(err) {
						t.Fatal("refusal created output directory")
					}
					continue
				}
				entries, err := os.ReadDir(output)
				if err != nil || len(entries) != 2 {
					t.Fatal("refusal changed output inventory")
				}
				for name, expected := range map[string]string{"generated.hpp": "header sentinel", "bindings.json": "receipt sentinel"} {
					data, err := os.ReadFile(filepath.Join(output, name))
					if err != nil || string(data) != expected {
						t.Fatal("refusal replaced existing output")
					}
				}
			}
		})
	}
}
