package cppbackend

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func selectedFloat64Emitter(t *testing.T) NativeEmitter {
	t.Helper()
	path := os.Getenv("PIPELANG_FLOAT64_EMITTER")
	if path == "" {
		t.Skip("explicit native emitter required for contained profile checks")
	}
	if !filepath.IsAbs(path) {
		t.Fatal("absolute native emitter path required")
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	hash := sha256.Sum256(data)
	return NativeEmitter{Path: path, SHA256: hex.EncodeToString(hash[:])}
}

func TestFloat64NativeProtocolRefusals(t *testing.T) {
	emitter := selectedFloat64Emitter(t)
	program := float64Program()
	request, err := projectFloat64Program(program)
	if err != nil {
		t.Fatal(err)
	}
	request.Functions[0].Body.Operands[1] = float64Expression{Kind: "literal", Type: "float64", Float64Bits: "8000000000000000"}
	encoded, err := json.Marshal(request)
	if err != nil {
		t.Fatal(err)
	}
	valid := string(encoded)
	cases := map[string]string{
		"unknown profile":         strings.Replace(valid, Float64ComparisonCapability, "unknown", 1),
		"wrong language":          strings.Replace(valid, "v0.113.0", "v0.1.0", 1),
		"duplicate profile":       strings.Replace(valid, `"profile":`, `"profile":"unknown","profile":`, 1),
		"case alias":              strings.Replace(valid, `"profile":`, `"Profile":`, 1),
		"null profile":            strings.Replace(valid, `"profile":"`+Float64ComparisonCapability+`"`, `"profile":null`, 1),
		"unknown member":          strings.Replace(valid, `"functions":`, `"unexpected":false,"functions":`, 1),
		"missing member":          strings.Replace(valid, `"compiler_contract":"pipelang.compiler.v1",`, "", 1),
		"duplicate literal":       strings.Replace(valid, `"float64_bits":`, `"float64_bits":"0000000000000000","float64_bits":`, 1),
		"null literal":            strings.Replace(valid, `"float64_bits":"8000000000000000"`, `"float64_bits":null`, 1),
		"numeric literal":         strings.Replace(valid, `"float64_bits":"8000000000000000"`, `"float64_bits":0`, 1),
		"uppercase bits":          strings.Replace(valid, "8000000000000000", "3FF0000000000000", 1),
		"short bits":              strings.Replace(valid, "8000000000000000", "0", 1),
		"infinite bits":           strings.Replace(valid, "8000000000000000", "7ff0000000000000", 1),
		"nan bits":                strings.Replace(valid, "8000000000000000", "fff8000000000001", 1),
		"decimal payload":         strings.Replace(valid, `"float64_bits":`, `"float":0.0,"float64_bits":`, 1),
		"negative reference":      strings.Replace(valid, `"parameter":0`, `"parameter":-1`, 1),
		"fractional reference":    strings.Replace(valid, `"parameter":0`, `"parameter":0.0`, 1),
		"exponent reference":      strings.Replace(valid, `"parameter":0`, `"parameter":0e0`, 1),
		"overflow reference":      strings.Replace(valid, `"parameter":0`, `"parameter":18446744073709551616`, 1),
		"outside reference":       strings.Replace(valid, `"parameter":0`, `"parameter":64`, 1),
		"wrong type":              strings.Replace(valid, `"type":"float64"`, `"type":"bool"`, 1),
		"null parameters":         strings.Replace(valid, `"parameters":["float64","float64"]`, `"parameters":null`, 1),
		"unknown operator":        strings.Replace(valid, `"operator":"less_or_equal"`, `"operator":"add"`, 1),
		"null operator":           strings.Replace(valid, `"operator":"less_or_equal"`, `"operator":null`, 1),
		"unknown function member": strings.Replace(valid, `"path":`, `"extra":0,"path":`, 1),
		"identity surrogate":      strings.Replace(valid, "app.Compare", `\ud800`, 1),
		"identity nul":            strings.Replace(valid, "app.Compare", `app\u0000Compare`, 1),
		"trailing":                valid + "{}",
		"deep":                    `{"contract":` + strings.Repeat("[", 514) + strings.Repeat("]", 514) + `}`,
		"oversized":               strings.Repeat(" ", maximumNativeRequest+1),
	}

	boundedRequests := map[string]func(*float64Request){
		"empty functions": func(request *float64Request) {
			request.Functions = []float64Function{}
		},
		"too many functions": func(request *float64Request) {
			request.Functions = make([]float64Function, 129)
		},
		"duplicate functions": func(request *float64Request) {
			request.Functions = append(request.Functions, request.Functions[0])
		},
		"too many parameters": func(request *float64Request) {
			request.Functions[0].Parameters = make([]string, 65)
		},
		"expression depth": func(request *float64Request) {
			for index := 0; index < 64; index++ {
				previous := request.Functions[0].Body
				request.Functions[0].Body = float64Expression{Kind: "unary", Type: "bool", Operator: "not", Operands: []float64Expression{previous}}
			}
		},
		"expression nodes": func(request *float64Request) {
			for index := 0; index < 11; index++ {
				previous := request.Functions[0].Body
				request.Functions[0].Body = float64Expression{Kind: "binary", Type: "bool", Operator: "and", Operands: []float64Expression{previous, previous}}
			}
		},
	}
	for name, mutate := range boundedRequests {
		var request float64Request
		if err := json.Unmarshal(encoded, &request); err != nil {
			t.Fatal(err)
		}
		mutate(&request)
		cases[name] = string(mustMarshalFloat64(t, request))
	}
	for name, input := range cases {
		t.Run(name, func(t *testing.T) {
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			defer cancel()
			command := exec.CommandContext(ctx, emitter.Path)
			command.Stdin = strings.NewReader(input)
			var output, diagnostic bytes.Buffer
			command.Stdout = &output
			command.Stderr = &diagnostic
			if err := command.Run(); err == nil || output.Len() != 0 || diagnostic.Len() == 0 {
				t.Fatalf("malformed request produced output or success: %v / %q / %q", err, output.String(), diagnostic.String())
			}
		})
	}
}

func TestFloat64NativeResponseAndProcessRefusals(t *testing.T) {
	selected := selectedFloat64Emitter(t)
	program := float64Program()
	request, err := projectFloat64Program(program)
	if err != nil {
		t.Fatal(err)
	}
	generated := generateFloat64(request, program)
	response := map[string]any{"contract": float64ResponseContract, "profile": Float64ComparisonCapability, "source": string(generated.Source),
		"functions": []map[string]string{{"package_id": request.Functions[0].PackageID, "path": request.Functions[0].Path, "name": float64Symbol(request.Functions[0])}}}
	encoded, err := json.Marshal(response)
	if err != nil {
		t.Fatal(err)
	}
	valid := string(encoded)
	cases := map[string]string{
		"unknown profile":   strings.Replace(valid, `"profile":"`+Float64ComparisonCapability+`"`, `"profile":"unknown"`, 1),
		"duplicate":         strings.Replace(valid, `"profile":`, `"profile":"unknown","profile":`, 1),
		"case alias":        strings.Replace(valid, `"source":`, `"Source":`, 1),
		"unknown field":     strings.Replace(valid, `"source":`, `"extra":0,"source":`, 1),
		"null source":       strings.Replace(valid, string(mustMarshalFloat64(t, string(generated.Source))), "null", 1),
		"bad identity":      strings.Replace(valid, "app.Compare", "other.Compare", 1),
		"binding duplicate": strings.Replace(valid, `"name":`, `"name":"wrong","name":`, 1),
		"surrogate":         strings.Replace(valid, "app.Compare", `\ud800`, 1),
		"trailing":          valid + "{}",
		"empty":             "",
	}
	for name, data := range cases {
		t.Run(name, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "emitter")
			script := []byte("#!/bin/sh\ncat <<'PROFILE_RESPONSE_END'\n" + data + "\nPROFILE_RESPONSE_END\n")
			if err := os.WriteFile(path, script, 0700); err != nil {
				t.Fatal(err)
			}
			hash := sha256.Sum256(script)
			emitter := NativeEmitter{Path: path, SHA256: hex.EncodeToString(hash[:])}
			if generated, err := emitter.GenerateWithProfile(context.Background(), program, Float64ComparisonCapability); err == nil || len(generated.Source) != 0 {
				t.Fatal("bad response returned source/fallback")
			}
		})
	}
	selected.SHA256 = strings.Repeat("0", 64)
	if generated, err := selected.GenerateWithProfile(context.Background(), program, Float64ComparisonCapability); err == nil || len(generated.Source) != 0 {
		t.Fatal("bad hash returned source")
	}
	selected = selectedFloat64Emitter(t)
	for _, expired := range []bool{false, true} {
		var ctx context.Context
		var cancel context.CancelFunc
		var expected error
		if expired {
			ctx, cancel = context.WithDeadline(context.Background(), time.Now().Add(-time.Second))
			expected = context.DeadlineExceeded
		} else {
			ctx, cancel = context.WithCancel(context.Background())
			cancel()
			expected = context.Canceled
		}
		generated, err := selected.GenerateWithProfile(ctx, program, Float64ComparisonCapability)
		cancel()
		if !errors.Is(err, expected) || len(generated.Source) != 0 {
			t.Fatalf("context refusal differed: %v", err)
		}
	}
}

func mustMarshalFloat64(t *testing.T, value any) []byte {
	t.Helper()
	encoded, err := json.Marshal(value)
	if err != nil {
		t.Fatal(err)
	}
	return encoded
}

func TestFloat64ResponseUTF8Boundary(t *testing.T) {
	program := float64Program()
	program.Functions[0].Identity.PackageID = "test.café"
	program.Functions[0].Identity.Path = "app.比較"
	request, err := projectFloat64Program(program)
	if err != nil {
		t.Fatal(err)
	}
	generated := generateFloat64(request, program)
	source := string(generated.Source) + "// café 日本語 🏍\n"
	response := map[string]any{
		"contract": float64ResponseContract,
		"profile":  Float64ComparisonCapability,
		"source":   source,
		"functions": []map[string]string{{
			"package_id": request.Functions[0].PackageID,
			"path":       request.Functions[0].Path,
			"name":       float64Symbol(request.Functions[0]),
		}},
	}
	valid := string(mustMarshalFloat64(t, response))
	cases := []struct {
		name     string
		response string
		accepted bool
	}{
		{"valid Unicode", valid, true},
		{"invalid source byte", strings.Replace(valid, `"source":"`, "\"source\":\"\xff", 1), false},
		{"invalid binding byte", strings.Replace(valid, "app.比較", "app.\xff", 1), false},
		{"truncated source sequence", strings.Replace(valid, "日本語", "\xe6\x97", 1), false},
	}
	for _, test := range cases {
		t.Run(test.name, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "emitter")
			script := []byte("#!/bin/sh\ncat <<'PROFILE_RESPONSE_END'\n" + test.response + "\nPROFILE_RESPONSE_END\n")
			if err := os.WriteFile(path, script, 0700); err != nil {
				t.Fatal(err)
			}
			hash := sha256.Sum256(script)
			emitter := NativeEmitter{Path: path, SHA256: hex.EncodeToString(hash[:])}
			actual, err := emitter.GenerateWithProfile(context.Background(), program, Float64ComparisonCapability)
			if test.accepted {
				if err != nil || string(actual.Source) != source || len(actual.Functions) != 1 || actual.Functions[0].Identity.Path != "app.比較" {
					t.Fatalf("valid Unicode response changed: %v", err)
				}
			} else if err == nil || len(actual.Source) != 0 || !strings.Contains(err.Error(), "exceeded its response contract") {
				t.Fatalf("invalid UTF-8 escaped shared transport admission: %v", err)
			}
		})
	}
}
