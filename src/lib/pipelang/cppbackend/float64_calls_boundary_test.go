package cppbackend

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func float64WireCall(target float64Function, arguments []float64Expression) float64Expression {
	return float64Expression{
		Kind: "call", Type: target.ReturnType,
		Callee:    &float64Callee{PackageID: target.PackageID, Path: target.Path},
		Arguments: &arguments,
	}
}

// Construct these graphs directly on the private wire, bypassing Core/projection
// checks so the native decoder must independently enforce every graph bound.
func float64WireChain(count int, branching bool) float64Request {
	request := float64Request{
		Contract: float64CallsRequestContract, Profile: Float64CallsCapability,
		LanguageContract: "v0.113.0", CompilerContract: "pipelang.compiler.v1",
	}
	for index := 0; index < count; index++ {
		function := float64Function{
			PackageID: "test.calls", Path: fmt.Sprintf("app.F%03d", index),
			Parameters: []string{}, ReturnType: "float64",
			Body: float64Expression{Kind: "literal", Type: "float64", Float64Bits: "8000000000000000"},
		}
		if index > 0 {
			call := float64WireCall(request.Functions[index-1], []float64Expression{})
			function.Body = call
			if branching {
				value := true
				function.Body = float64Expression{
					Kind: "conditional", Type: "float64",
					Operands: []float64Expression{
						{Kind: "literal", Type: "bool", Boolean: &value}, call, call,
					},
				}
			}
		}
		request.Functions = append(request.Functions, function)
	}
	return request
}

func TestFloat64CallsNativeIndependentGraphBounds(t *testing.T) {
	emitter := selectedFloat64Emitter(t)
	base, err := projectFloat64ProgramWithProfile(float64CallsProgram(), Float64CallsCapability)
	if err != nil {
		t.Fatal(err)
	}
	encoded := mustMarshalFloat64(t, base)
	clone := func() float64Request {
		var request float64Request
		if err := json.Unmarshal(encoded, &request); err != nil {
			t.Fatal(err)
		}
		return request
	}
	mutual := clone()
	arguments := *mutual.Functions[0].Body.Arguments
	mutual.Functions[1].Body = float64WireCall(mutual.Functions[0], arguments)
	crossOwner := clone()
	crossOwner.Functions[1].Path = "other.Compare"
	crossOwner.Functions[0].Body.Callee.Path = "other.Compare"
	missingArguments := clone()
	missingArguments.Functions[0].Body.Arguments = nil
	// The active call shape requires an actual argument array, including zero arity.
	nilArguments := clone()
	nilSlice := []float64Expression(nil)
	nilArguments.Functions[0].Body.Arguments = &nilSlice
	cases := map[string]float64Request{
		"mutual recursion":  mutual,
		"cross owner":       crossOwner,
		"missing arguments": missingArguments,
		"null arguments":    nilArguments,
		"depth 65":          float64WireChain(65, false),
		"expanded work":     float64WireChain(14, true),
	}
	for name, request := range cases {
		t.Run(name, func(t *testing.T) {
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			defer cancel()
			command := exec.CommandContext(ctx, emitter.Path)
			command.Stdin = bytes.NewReader(mustMarshalFloat64(t, request))
			var output, diagnostic bytes.Buffer
			command.Stdout = &output
			command.Stderr = &diagnostic
			if err := command.Run(); err == nil || output.Len() != 0 || diagnostic.Len() == 0 {
				t.Fatalf("invalid native-only call graph produced output or success: %v", err)
			}
		})
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	command := exec.CommandContext(ctx, emitter.Path)
	command.Stdin = bytes.NewReader(mustMarshalFloat64(t, float64WireChain(64, false)))
	var output, diagnostic bytes.Buffer
	command.Stdout = &output
	command.Stderr = &diagnostic
	if err := command.Run(); err != nil || output.Len() == 0 || diagnostic.Len() != 0 {
		t.Fatalf("bounded zero-argument forward graph refused: %v / %s", err, diagnostic.String())
	}
}

func TestFloat64CallsNativeSelectedResponseAndProcessRefusals(t *testing.T) {
	selected := selectedFloat64Emitter(t)
	program := float64CallsProgram()
	generated, err := selected.GenerateWithProfile(context.Background(), program, Float64CallsCapability)
	if err != nil {
		t.Fatal(err)
	}
	request, err := projectFloat64ProgramWithProfile(program, Float64CallsCapability)
	if err != nil {
		t.Fatal(err)
	}
	bindings := make([]map[string]string, 0, len(request.Functions))
	for _, function := range request.Functions {
		bindings = append(bindings, map[string]string{
			"package_id": function.PackageID, "path": function.Path, "name": float64Symbol(function),
		})
	}
	response := map[string]any{
		"contract": float64CallsResponseContract, "profile": Float64CallsCapability,
		"source": string(generated.Source), "functions": bindings,
	}
	valid := string(mustMarshalFloat64(t, response))
	sourceJSON := string(mustMarshalFloat64(t, string(generated.Source)))
	cases := map[string]string{
		"v3 response":         strings.Replace(valid, float64CallsResponseContract, float64ConditionalResponseContract, 1),
		"wrong profile":       strings.Replace(valid, `"profile":"`+Float64CallsCapability+`"`, `"profile":"`+Float64ConditionalCapability+`"`, 1),
		"duplicate profile":   strings.Replace(valid, `"profile":`, `"profile":"unknown","profile":`, 1),
		"unknown member":      strings.Replace(valid, `"source":`, `"extra":false,"source":`, 1),
		"missing source":      strings.Replace(valid, `"source":`+sourceJSON, `"unexpected":`+sourceJSON, 1),
		"empty source":        strings.Replace(valid, sourceJSON, `""`, 1),
		"null source":         strings.Replace(valid, sourceJSON, `null`, 1),
		"binding path":        strings.Replace(valid, `"path":"app.Caller"`, `"path":"app.Other"`, 1),
		"binding symbol":      strings.Replace(valid, `"name":`, `"name":"unexpected","name":`, 1),
		"invalid source UTF8": strings.Replace(valid, `"source":"`, "\"source\":\"\xff", 1),
		"surrogate binding":   strings.Replace(valid, `"path":"app.Caller"`, `"path":"\ud800"`, 1),
		"trailing":            valid + "{}",
		"empty":               "",
	}
	for name, response := range cases {
		t.Run(name, func(t *testing.T) {
			if response == valid {
				t.Fatal("response refusal mutation did not change input")
			}
			path := filepath.Join(t.TempDir(), "emitter")
			script := []byte("#!/bin/sh\ncat <<'CALLS_RESPONSE_END'\n" + response + "\nCALLS_RESPONSE_END\n")
			if err := os.WriteFile(path, script, 0700); err != nil {
				t.Fatal(err)
			}
			hash := sha256.Sum256(script)
			emitter := NativeEmitter{Path: path, SHA256: hex.EncodeToString(hash[:])}
			generated, err := emitter.GenerateWithProfile(context.Background(), program, Float64CallsCapability)
			if err == nil || len(generated.Source) != 0 {
				t.Fatal("invalid selected v4 response produced output or fallback")
			}
		})
	}
	for name, script := range map[string]string{
		"process failure": "#!/bin/sh\nprintf 'deliberate failure' >&2\nexit 7\n",
		"truncated JSON":  "#!/bin/sh\nprintf '{'\n",
		"stderr response": "#!/bin/sh\nprintf 'unexpected diagnostic' >&2\n",
	} {
		t.Run(name, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "emitter")
			if err := os.WriteFile(path, []byte(script), 0700); err != nil {
				t.Fatal(err)
			}
			hash := sha256.Sum256([]byte(script))
			emitter := NativeEmitter{Path: path, SHA256: hex.EncodeToString(hash[:])}
			generated, err := emitter.GenerateWithProfile(context.Background(), program, Float64CallsCapability)
			if err == nil || len(generated.Source) != 0 {
				t.Fatal("failed selected v4 process produced output or fallback")
			}
		})
	}
	selected.SHA256 = strings.Repeat("0", 64)
	if generated, err := selected.GenerateWithProfile(context.Background(), program, Float64CallsCapability); err == nil || len(generated.Source) != 0 {
		t.Fatal("wrong executable checksum produced output")
	}
	selected = selectedFloat64Emitter(t)
	for _, deadline := range []bool{false, true} {
		var ctx context.Context
		var cancel context.CancelFunc
		expected := error(context.Canceled)
		if deadline {
			ctx, cancel = context.WithDeadline(context.Background(), time.Now().Add(-time.Second))
			expected = context.DeadlineExceeded
		} else {
			ctx, cancel = context.WithCancel(context.Background())
			cancel()
		}
		generated, err := selected.GenerateWithProfile(ctx, program, Float64CallsCapability)
		cancel()
		if !errors.Is(err, expected) || len(generated.Source) != 0 {
			t.Fatalf("selected v4 context refusal differs: %v", err)
		}
	}
}
