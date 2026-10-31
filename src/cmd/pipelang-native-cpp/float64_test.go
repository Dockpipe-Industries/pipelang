package main

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang/coreeval"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang/coreir"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang/cppbackend"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang/gobackend"
	"github.com/Dockpipe-Industries/pipelang/tests/containedexec"
)

const comparisonSource = `public Class MaterialAdmission {
    public bool Equal(float left, float right) => left == right;
    public bool NotEqual(float left, float right) => left != right;
    public bool Less(float left, float right) => left < right;
    public bool LessEqual(float left, float right) => left <= right;
    public bool Greater(float left, float right) => left > right;
    public bool GreaterEqual(float left, float right) => left >= right;
    public bool WithinClosedRange(float value, float minimum, float maximum)
        => value >= minimum && value <= maximum;
    public bool ContactOrderValid(float dynamicFriction, float staticFriction)
        => dynamicFriction <= staticFriction;
    public bool Compose(bool first, bool second, float left, float right)
        => !first || (second && left <= right);
    public bool BoolEqual(bool left, bool right) => left == right;
    public bool BoolNotEqual(bool left, bool right) => left != right;
    public bool UnitInterval(float value) => value >= 0.0 && value <= 1.0;
}`

func compileFloat64Fixture(t *testing.T, source string) (coreir.Program, cppbackend.Generated, metadata) {
	t.Helper()
	var program coreir.Program
	var generated cppbackend.Generated
	emit := func(admitted coreir.Program) (cppbackend.Generated, error) {
		program = admitted
		var err error
		generated, err = cppbackend.GenerateWithProfile(admitted, cppbackend.Float64ComparisonCapability)
		return generated, err
	}
	sourceBytes, receipt, err := compileWithProfileComponents([]byte(source), "Material.pipe", "example.material", "app.material", cppbackend.Float64ComparisonCapability, emit, nil)
	if err != nil || !bytes.Equal(sourceBytes, generated.Source) {
		t.Fatalf("float64 source pipeline: %v", err)
	}
	return program, generated, receipt
}

func TestFloat64SourceProfileAndRefusals(t *testing.T) {
	_, generated, receipt := compileFloat64Fixture(t, comparisonSource)
	if receipt.Profile != cppbackend.Float64ComparisonCapability || receipt.SourceSHA256 != digest([]byte(comparisonSource)) || receipt.GeneratedSHA256 != digest(generated.Source) || len(receipt.Functions) != 12 {
		t.Fatal("selected profile export identities differ")
	}
	if _, _, err := compile([]byte(comparisonSource), "Material.pipe", "example.material", "app.material"); err == nil {
		t.Fatal("default pilot silently admitted float64")
	}
	for _, source := range []string{
		`public Class X { public float Return(float x) => x; }`,
		`public Class X { public bool Compare(int x, int y) => x < y; }`,
		`public Class X { public bool Compare(float x) => x < 1; }`,
		`public Class X { public bool Compare(float x) => x < -1.0; }`,
		`public Class X { public Result<float, ArithmeticError> Divide(float x, float y) => x / y; }`,
		`public Class X { public bool Helper(float x) => x < 1.0; public bool Call(float x) => Helper(x); }`,
		`public Class X { public bool Local(float x) { bool result = x < 1.0; return result; } }`,
		`public Class X { public bool Choice(bool x) => x ? true : false; }`,
		`public Record R { public float X; } public Class X { public bool Read(R value) => value.X < 1.0; }`,
	} {
		_, _, err := compileWithProfileComponents([]byte(source), "Refuse.pipe", "example.material", "app.material", cppbackend.Float64ComparisonCapability,
			func(program coreir.Program) (cppbackend.Generated, error) {
				return cppbackend.GenerateWithProfile(program, cppbackend.Float64ComparisonCapability)
			}, nil)
		if err == nil {
			t.Fatalf("unsupported source admitted: %s", source)
		}
	}
}

func float64NativeTools(t *testing.T) (cppbackend.NativeEmitter, string, string) {
	t.Helper()
	path := os.Getenv("PIPELANG_FLOAT64_EMITTER")
	cxx := os.Getenv("PIPELANG_FLOAT64_CXX")
	goTool := os.Getenv("PIPELANG_FLOAT64_GO")
	if path == "" && cxx == "" && goTool == "" {
		t.Skip("explicit float64 emitter/C++/Go tools required for contained execution")
	}
	for _, path := range []string{path, cxx, goTool} {
		if !filepath.IsAbs(path) {
			t.Fatal("every selected float64 test tool must be an absolute path")
		}
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	hash := sha256.Sum256(data)
	return cppbackend.NativeEmitter{Path: path, SHA256: hex.EncodeToString(hash[:])}, cxx, goTool
}

func float64ProofDirectory(t *testing.T) string {
	t.Helper()
	root := os.Getenv("PIPELANG_FLOAT64_PROOF")
	if root == "" {
		return t.TempDir()
	}
	if !filepath.IsAbs(root) {
		t.Fatal("float64 proof directory must be absolute")
	}
	directory := filepath.Join(root, t.Name())
	if err := os.MkdirAll(directory, 0700); err != nil {
		t.Fatal(err)
	}
	return directory
}

func writeFloat64Proof(t *testing.T, path string, data []byte) {
	t.Helper()
	if err := os.WriteFile(path, data, 0600); err != nil {
		t.Fatal(err)
	}
}

func runFloat64Proof(t *testing.T, command *exec.Cmd) []byte {
	t.Helper()
	output, err := containedexec.CombinedOutput(command)
	if err != nil {
		t.Fatalf("contained float64 check failed: %v\n%s", err, output)
	}
	return output
}

func TestFloat64IEEEExecutionParity(t *testing.T) {
	emitter, cxx, goTool := float64NativeTools(t)
	program, generated, _ := compileFloat64Fixture(t, comparisonSource)
	native, err := emitter.GenerateWithProfile(context.Background(), program, cppbackend.Float64ComparisonCapability)
	if err != nil || !bytes.Equal(native.Source, generated.Source) || !reflect.DeepEqual(native.Functions, generated.Functions) {
		t.Fatalf("native/reference header or binding mismatch: %v", err)
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
		for _, binding := range generated.Functions {
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
	caseCount := 0
	check := func(method string, floats []float64, booleans []bool, expected bool) {
		function := functions[method]
		arguments := make([]coreeval.Value, 0, len(function.Parameters))
		var cppArgs, goArgs []string
		floatIndex, boolIndex := 0, 0
		for _, parameter := range function.Parameters {
			value := coreeval.Value{Type: parameter.Type}
			if parameter.Type.Kind == coreir.TypeNumeric {
				value.Float = floats[floatIndex]
				floatIndex++
				bits := math.Float64bits(value.Float)
				cppArgs = append(cppArgs, fmt.Sprintf("from_bits(UINT64_C(0x%016x))", bits))
				goArgs = append(goArgs, fmt.Sprintf("math.Float64frombits(0x%016x)", bits))
			} else {
				value.Bool = booleans[boolIndex]
				boolIndex++
				cppArgs = append(cppArgs, fmt.Sprint(value.Bool))
				goArgs = append(goArgs, fmt.Sprint(value.Bool))
			}
			arguments = append(arguments, value)
		}
		outcome, err := coreeval.EvaluateProgram(program, function.Identity, arguments)
		if err != nil || !outcome.OK || outcome.Value.Bool != expected {
			t.Fatalf("independent oracle/evaluator mismatch at %s/%d: %#v / %v", method, caseCount, outcome, err)
		}
		caseCount++
		fmt.Fprintf(&cppChecks, "    if (pipelang::%s(%s) != %t) { std::cerr << \"case %d\\n\"; return 1; }\n", cppNames[method], strings.Join(cppArgs, ", "), expected, caseCount)
		fmt.Fprintf(&goChecks, "    if %s(%s) != %t { t.Fatal(\"case %d\") }\n", goNames[method], strings.Join(goArgs, ", "), expected, caseCount)
	}
	values := []float64{math.Inf(-1), -math.MaxFloat64, -1, -math.SmallestNonzeroFloat64, math.Copysign(0, -1), 0,
		math.SmallestNonzeroFloat64, math.Nextafter(1, 0), 1, math.Nextafter(1, 2), math.MaxFloat64, math.Inf(1), math.NaN(), math.Float64frombits(0xfff8000000000001)}
	for _, left := range values {
		for _, right := range values {
			check("Equal", []float64{left, right}, nil, left == right)
			check("NotEqual", []float64{left, right}, nil, left != right)
			check("Less", []float64{left, right}, nil, left < right)
			check("LessEqual", []float64{left, right}, nil, left <= right)
			check("Greater", []float64{left, right}, nil, left > right)
			check("GreaterEqual", []float64{left, right}, nil, left >= right)
			check("ContactOrderValid", []float64{left, right}, nil, left <= right)
			for _, first := range []bool{false, true} {
				for _, second := range []bool{false, true} {
					check("Compose", []float64{left, right}, []bool{first, second}, !first || (second && left <= right))
				}
			}
		}
		check("UnitInterval", []float64{left}, nil, left >= 0 && left <= 1)
		for _, bounds := range [][2]float64{{0, 1}, {0, 10}, {-1, 0.5}, {0, 1e30}, {10, 0}} {
			check("WithinClosedRange", []float64{left, bounds[0], bounds[1]}, nil, left >= bounds[0] && left <= bounds[1])
		}
	}
	for _, left := range []bool{false, true} {
		for _, right := range []bool{false, true} {
			check("BoolEqual", nil, []bool{left, right}, left == right)
			check("BoolNotEqual", nil, []bool{left, right}, left != right)
		}
	}
	directory := float64ProofDirectory(t)
	writeFloat64Proof(t, filepath.Join(directory, "reference.hpp"), generated.Source)
	writeFloat64Proof(t, filepath.Join(directory, "native.hpp"), native.Source)
	cppMain := "#include HEADER\n#include <iostream>\nusing pipelang::float64_comparison_detail::from_bits;\nint main() {\n" + cppChecks.String() + "    return 0;\n}\n"
	writeFloat64Proof(t, filepath.Join(directory, "main.cpp"), []byte(cppMain))
	for _, version := range []string{"reference", "native"} {
		binary := filepath.Join(directory, version)
		command := exec.Command(cxx, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", `-DHEADER="`+version+`.hpp"`, filepath.Join(directory, "main.cpp"), "-o", binary)
		runFloat64Proof(t, command)
		runFloat64Proof(t, exec.Command(binary))
	}
	goDirectory := filepath.Join(directory, "go")
	if err := os.MkdirAll(goDirectory, 0700); err != nil {
		t.Fatal(err)
	}
	writeFloat64Proof(t, filepath.Join(goDirectory, "generated.go"), goGenerated.Source)
	writeFloat64Proof(t, filepath.Join(goDirectory, "generated_test.go"), []byte("package pipelanggenerated\nimport (\"math\"; \"testing\")\nfunc TestMatrix(t *testing.T) {\n"+goChecks.String()+"}\n"))
	command := exec.Command(goTool, "test", "-p", "1", "-count=1")
	command.Dir = goDirectory
	command.Env = append(os.Environ(), "GO111MODULE=off")
	runFloat64Proof(t, command)
	for _, flag := range []string{"-ffast-math", "-ffinite-math-only"} {
		command := exec.Command(cxx, "-std=c++17", flag, `-DHEADER="reference.hpp"`, "-fsyntax-only", filepath.Join(directory, "main.cpp"))
		output, err := containedexec.CombinedOutput(command)
		if err == nil || !bytes.Contains(output, []byte("require strict IEEE")) {
			t.Fatalf("unsafe math flag %s did not fail closed: %v\n%s", flag, err, output)
		}
	}
	t.Logf("%d cases matched evaluator, generated Go, Go-emitted C++ and native-emitted C++; unsafe flags refused", caseCount)
}

func TestFloat64FiniteLiteralExecution(t *testing.T) {
	emitter, cxx, goTool := float64NativeTools(t)
	program, _, _ := compileFloat64Fixture(t, `public Class Literal { public bool Match(float value) => value == 0.0; }`)
	original := program.Functions[0]
	program.Functions = nil
	bits := []uint64{0, 0x8000000000000000, 1, 0x8000000000000001, 0x0010000000000000,
		0x8010000000000000, 0x7fefffffffffffff, 0xffefffffffffffff, 0x3fb999999999999a, 0xbff0000000000000}
	for index, value := range bits {
		function := original
		function.Name = fmt.Sprintf("Literal%d", index)
		function.Identity.Path = original.Identity.Path + fmt.Sprintf(".literal%d", index)
		binary := *original.Body.Binary
		literal := *original.Body.Binary.Right
		literal.Literal = &coreir.Literal{Float: math.Float64frombits(value)}
		binary.Right = &literal
		function.Body.Binary = &binary
		program.Functions = append(program.Functions, function)
	}
	reference, err := cppbackend.GenerateWithProfile(program, cppbackend.Float64ComparisonCapability)
	if err != nil {
		t.Fatal(err)
	}
	native, err := emitter.GenerateWithProfile(context.Background(), program, cppbackend.Float64ComparisonCapability)
	if err != nil || !bytes.Equal(reference.Source, native.Source) || !reflect.DeepEqual(reference.Functions, native.Functions) {
		t.Fatalf("exact literal/header parity failed: %v", err)
	}
	goGenerated, err := gobackend.GenerateWithNames(program)
	if err != nil {
		t.Fatal(err)
	}
	var cppChecks, goChecks strings.Builder
	for index, function := range program.Functions {
		cppName, goName := "", ""
		for _, binding := range reference.Functions {
			if binding.Identity.Path == function.Identity.Path {
				cppName = binding.Name
			}
		}
		for _, binding := range goGenerated.Functions {
			if binding.Identity.Path == function.Identity.Path {
				goName = binding.Name
			}
		}
		literal := math.Float64frombits(bits[index])
		if !bytes.Contains(reference.Source, []byte(fmt.Sprintf("from_bits(UINT64_C(0x%016x))", bits[index]))) {
			t.Fatal("emitted literal did not retain exact finite bits")
		}
		fmt.Fprintf(&cppChecks, "    { const double value = from_bits(UINT64_C(0x%016x)); std::uint64_t actual; std::memcpy(&actual, &value, sizeof(value)); if (actual != UINT64_C(0x%016x)) return 2; }\n", bits[index], bits[index])
		for _, input := range bits {
			value := math.Float64frombits(input)
			expected := value == literal
			outcome, err := coreeval.EvaluateProgram(program, function.Identity, []coreeval.Value{{Type: coreir.BinaryFloat(64), Float: value}})
			if err != nil || !outcome.OK || outcome.Value.Bool != expected {
				t.Fatalf("finite literal oracle differs: %v", err)
			}
			fmt.Fprintf(&cppChecks, "    if (pipelang::%s(from_bits(UINT64_C(0x%016x))) != %t) return 1;\n", cppName, input, expected)
			fmt.Fprintf(&goChecks, "    if %s(math.Float64frombits(0x%016x)) != %t { t.Fatal(\"literal case\") }\n", goName, input, expected)
		}
	}
	directory := float64ProofDirectory(t)
	writeFloat64Proof(t, filepath.Join(directory, "reference.hpp"), reference.Source)
	writeFloat64Proof(t, filepath.Join(directory, "native.hpp"), native.Source)
	writeFloat64Proof(t, filepath.Join(directory, "main.cpp"), []byte("#include HEADER\nusing pipelang::float64_comparison_detail::from_bits;\nint main() {\n"+cppChecks.String()+"    return 0;\n}\n"))
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
	writeFloat64Proof(t, filepath.Join(goDirectory, "generated_test.go"), []byte("package pipelanggenerated\nimport (\"math\"; \"testing\")\nfunc TestLiterals(t *testing.T) {\n"+goChecks.String()+"}\n"))
	command := exec.Command(goTool, "test", "-p", "1", "-count=1")
	command.Dir = goDirectory
	command.Env = append(os.Environ(), "GO111MODULE=off")
	runFloat64Proof(t, command)
	t.Log("100 finite-literal comparisons and ten exact bit reconstructions passed across both emitters; generated Go/evaluator bool results agree")
}

func TestFloat64GeneratedShortCircuit(t *testing.T) {
	emitter, cxx, _ := float64NativeTools(t)
	program, reference, _ := compileFloat64Fixture(t, `public Class Gates {
    public bool And(bool gate, float value) => gate && value < 1.0;
    public bool Or(bool gate, float value) => gate || value < 1.0;
}`)
	native, err := emitter.GenerateWithProfile(context.Background(), program, cppbackend.Float64ComparisonCapability)
	if err != nil || !bytes.Equal(reference.Source, native.Source) {
		t.Fatalf("short-circuit headers differ: %v", err)
	}
	// Instrument only the test copy of the generated literal reconstruction.
	// A skipped RHS must never request its literal, including with a NaN input.
	instrumented := strings.Replace(string(reference.Source), "namespace pipelang::float64_comparison_detail {", "namespace pipelang::float64_comparison_detail {\ninline int literal_reads = 0;", 1)
	instrumented = strings.Replace(instrumented, "    double value;", "    ++literal_reads;\n    double value;", 1)
	names := make(map[string]string)
	for _, function := range program.Functions {
		for _, binding := range reference.Functions {
			if function.Identity.Path == binding.Identity.Path {
				names[function.Name] = binding.Name
			}
		}
	}
	main := fmt.Sprintf(`#include "generated.hpp"
#include <limits>
int main() {
    using namespace pipelang::float64_comparison_detail;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    if (pipelang::%s(false, nan) || literal_reads != 0) return 1;
    if (!pipelang::%s(true, nan) || literal_reads != 0) return 2;
    if (!pipelang::%s(true, 0.5) || literal_reads != 1) return 3;
    if (pipelang::%s(false, 2.0) || literal_reads != 2) return 4;
    return 0;
}
`, names["And"], names["Or"], names["And"], names["Or"])
	directory := float64ProofDirectory(t)
	writeFloat64Proof(t, filepath.Join(directory, "generated.hpp"), []byte(instrumented))
	writeFloat64Proof(t, filepath.Join(directory, "main.cpp"), []byte(main))
	binary := filepath.Join(directory, "trace")
	runFloat64Proof(t, exec.Command(cxx, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", filepath.Join(directory, "main.cpp"), "-o", binary))
	runFloat64Proof(t, exec.Command(binary))
}

func TestFloat64SelectionPreservesPilotArtifacts(t *testing.T) {
	emitter, _, _ := float64NativeTools(t)
	baseline := os.Getenv("PIPELANG_FLOAT64_BASELINE_FRONTEND")
	current := os.Getenv("PIPELANG_FLOAT64_FRONTEND")
	if baseline == "" || current == "" {
		t.Skip("explicit immutable baseline and current frontends required")
	}
	if !filepath.IsAbs(baseline) || !filepath.IsAbs(current) {
		t.Fatal("frontend comparison requires absolute paths")
	}
	cases := []string{
		`public Class Root { public Result<int,ArithmeticError> Add(int a,int b)=>a+b; public Result<int,ArithmeticError> Negate(int value)=>-value; }`,
		`public Class Root { public bool Mark(bool value)=>value; public bool Choose(bool first,bool second)=>Mark(first)&&Mark(second); }`,
		`public Record Row { public string Text; public bool Active; } public Class Root { public Row Make(string text,bool active)=>new Row {Text=text,Active=active}; public bool Before(string first,string second)=>first<second; }`,
		`public Record Row { public string Text; public bool Active; } public Class Root { public List<Row> One(Row row)=>list(row); public Optional<Row> At(List<Row> rows,int index)=>at(rows,index); public List<Row> Filter(List<Row> rows,string text)=>filter_by(rows,Row.Text,text); }`,
	}
	directory := float64ProofDirectory(t)
	for index, source := range cases {
		sourcePath := filepath.Join(directory, fmt.Sprintf("pilot%d.pipe", index))
		writeFloat64Proof(t, sourcePath, []byte(source))
		var expectedHeader []byte
		var expectedReceipt map[string]any
		for _, selection := range []string{"baseline", "default", "explicit-pilot", "native-pilot"} {
			output := filepath.Join(directory, fmt.Sprintf("pilot%d-%s", index, selection))
			tool := current
			if selection == "baseline" {
				tool = baseline
			}
			arguments := []string{"--source", sourcePath, "--output-dir", output, "--package", "example.preserve", "--module", "app.preserve"}
			if selection == "explicit-pilot" {
				arguments = append(arguments, "--profile", cppbackend.Capability)
			}
			if selection == "native-pilot" {
				arguments = append(arguments, "--cpp-emitter", emitter.Path, "--cpp-emitter-sha256", emitter.SHA256)
			}
			runFloat64Proof(t, exec.Command(tool, arguments...))
			header, err := os.ReadFile(filepath.Join(output, "generated.hpp"))
			if err != nil {
				t.Fatal(err)
			}
			receiptBytes, err := os.ReadFile(filepath.Join(output, "bindings.json"))
			if err != nil {
				t.Fatal(err)
			}
			var receipt map[string]any
			if err := json.Unmarshal(receiptBytes, &receipt); err != nil {
				t.Fatal(err)
			}
			delete(receipt, "compiler_executable_sha256")
			delete(receipt, "emitter_executable_sha256")
			if selection == "baseline" {
				expectedHeader = header
				expectedReceipt = receipt
			} else if !bytes.Equal(header, expectedHeader) || !reflect.DeepEqual(receipt, expectedReceipt) {
				t.Fatalf("pilot output changed for case %d / %s", index, selection)
			}
		}
	}
}

func TestFloat64CLIRefusesBeforeWriting(t *testing.T) {
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
		{"unknown-profile", comparisonSource, []string{"--profile", "unknown"}},
		{"default-pilot", comparisonSource, nil},
		{"missing-hash", comparisonSource, []string{"--profile", cppbackend.Float64ComparisonCapability, "--cpp-emitter", emitter.Path}},
		{"missing-path", comparisonSource, []string{"--profile", cppbackend.Float64ComparisonCapability, "--cpp-emitter-sha256", emitter.SHA256}},
		{"wrong-hash", comparisonSource, []string{"--profile", cppbackend.Float64ComparisonCapability, "--cpp-emitter", emitter.Path, "--cpp-emitter-sha256", strings.Repeat("0", 64)}},
		{"process-failure", comparisonSource, []string{"--profile", cppbackend.Float64ComparisonCapability, "--cpp-emitter", failedEmitter, "--cpp-emitter-sha256", hex.EncodeToString(failedHash[:])}},
		{"unsupported-shape", `public Class Root {public float Add(float left,float right)=>left+right;}`, []string{"--profile", cppbackend.Float64ComparisonCapability}},
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
