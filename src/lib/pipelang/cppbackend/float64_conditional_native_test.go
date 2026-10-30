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

func TestFloat64ConditionalNativeProtocolRefusals(t *testing.T) {
	emitter := selectedFloat64Emitter(t)
	request, err := projectFloat64ProgramWithProfile(float64ConditionalProgram(), Float64ConditionalCapability)
	if err != nil {
		t.Fatal(err)
	}
	valid := string(mustMarshalFloat64(t, request))
	cases := map[string]string{
		"unknown profile":                      strings.Replace(valid, Float64ConditionalCapability, "unknown", 1),
		"comparison profile with v3":           strings.Replace(valid, Float64ConditionalCapability, Float64ComparisonCapability, 1),
		"v2 contract with conditional profile": strings.Replace(valid, "pipelang.cpp-emitter.request.v3", "pipelang.cpp-emitter.request.v2", 1),
		"duplicate profile":                    strings.Replace(valid, `"profile":`, `"profile":"unknown","profile":`, 1),
		"missing return type":                  strings.Replace(valid, `"return_type":"float64",`, "", 1),
		"unknown return type":                  strings.Replace(valid, `"return_type":"float64"`, `"return_type":"float32"`, 1),
		"null return type":                     strings.Replace(valid, `"return_type":"float64"`, `"return_type":null`, 1),
		"numeric return type":                  strings.Replace(valid, `"return_type":"float64"`, `"return_type":0`, 1),
		"duplicate return type":                strings.Replace(valid, `"return_type":`, `"return_type":"bool","return_type":`, 1),
		"return type case alias":               strings.Replace(valid, `"return_type":`, `"Return_Type":`, 1),
		"body return mismatch":                 strings.Replace(valid, `"return_type":"float64"`, `"return_type":"bool"`, 1),
		"conditional unknown member":           strings.Replace(valid, `"kind":"conditional",`, `"kind":"conditional","extra":false,`, 1),
		"conditional terminal member":          strings.Replace(valid, `"kind":"conditional",`, `"kind":"conditional","terminal_statement":true,`, 1),
		"conditional inactive operator":        strings.Replace(valid, `"kind":"conditional",`, `"kind":"conditional","operator":"less_than",`, 1),
		"conditional duplicate kind":           strings.Replace(valid, `"kind":"conditional",`, `"kind":"conditional","kind":"conditional",`, 1),
		"trailing":                             valid + "{}",
		"oversized":                            strings.Repeat(" ", maximumNativeRequest+1),
	}
	mutations := map[string]func(*float64Request){
		"missing operands": func(request *float64Request) {
			request.Functions[0].Body.Operands = nil
		},
		"two operands": func(request *float64Request) {
			request.Functions[0].Body.Operands = request.Functions[0].Body.Operands[:2]
		},
		"four operands": func(request *float64Request) {
			body := &request.Functions[0].Body
			body.Operands = append(body.Operands, body.Operands[2])
		},
		"numeric condition": func(request *float64Request) {
			body := &request.Functions[0].Body
			body.Operands[0] = body.Operands[1]
		},
		"bool arm": func(request *float64Request) {
			body := &request.Functions[0].Body
			body.Operands[1] = body.Operands[0]
		},
		"bool conditional result": func(request *float64Request) {
			request.Functions[0].Body.Type = "bool"
		},
		"arithmetic": func(request *float64Request) {
			request.Functions[0].Body.Operands[0].Operator = "add"
		},
		"nonfinite literal arm": func(request *float64Request) {
			request.Functions[0].Body.Operands[1] = float64Expression{Kind: "literal", Type: "float64", Float64Bits: "7ff8000000000001"}
		},
		"depth": func(request *float64Request) {
			for index := 0; index < 64; index++ {
				previous := request.Functions[0].Body
				request.Functions[0].Body = float64Expression{
					Kind: "conditional", Type: "float64",
					Operands: []float64Expression{previous.Operands[0], previous, previous.Operands[2]},
				}
			}
		},
		"nodes": func(request *float64Request) {
			for index := 0; index < 12; index++ {
				previous := request.Functions[0].Body
				request.Functions[0].Body = float64Expression{
					Kind: "conditional", Type: "float64",
					Operands: []float64Expression{previous.Operands[0], previous, previous},
				}
			}
		},
	}
	for name, mutate := range mutations {
		var mutated float64Request
		if err := json.Unmarshal([]byte(valid), &mutated); err != nil {
			t.Fatal(err)
		}
		mutate(&mutated)
		cases[name] = string(mustMarshalFloat64(t, mutated))
	}
	for name, input := range cases {
		t.Run(name, func(t *testing.T) {
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			defer cancel()
			command := exec.CommandContext(ctx, emitter.Path)
			command.Stdin = strings.NewReader(input)
			var output bytes.Buffer
			var diagnostic bytes.Buffer
			command.Stdout = &output
			command.Stderr = &diagnostic
			if err := command.Run(); err == nil || output.Len() != 0 || diagnostic.Len() == 0 {
				t.Fatalf("malformed v3 request produced output or success: %v / %q / %q", err, output.String(), diagnostic.String())
			}
		})
	}
}

func TestFloat64ConditionalNativeResponseAndProcessRefusals(t *testing.T) {
	selected := selectedFloat64Emitter(t)
	program := float64ConditionalProgram()
	request, err := projectFloat64ProgramWithProfile(program, Float64ConditionalCapability)
	if err != nil {
		t.Fatal(err)
	}
	generated, err := GenerateWithProfile(program, Float64ConditionalCapability)
	if err != nil {
		t.Fatal(err)
	}
	response := map[string]any{
		"contract": "pipelang.cpp-emitter.response.v3",
		"profile":  Float64ConditionalCapability,
		"source":   string(generated.Source),
		"functions": []map[string]string{{
			"package_id": request.Functions[0].PackageID,
			"path":       request.Functions[0].Path,
			"name":       float64Symbol(request.Functions[0]),
		}},
	}
	valid := string(mustMarshalFloat64(t, response))
	cases := map[string]string{
		"v2 response":                 strings.Replace(valid, "pipelang.cpp-emitter.response.v3", "pipelang.cpp-emitter.response.v2", 1),
		"comparison response profile": strings.Replace(valid, Float64ConditionalCapability, Float64ComparisonCapability, 1),
		"unknown profile":             strings.Replace(valid, `"profile":"`+Float64ConditionalCapability+`"`, `"profile":"unknown"`, 1),
		"duplicate profile":           strings.Replace(valid, `"profile":`, `"profile":"unknown","profile":`, 1),
		"unknown member":              strings.Replace(valid, `"source":`, `"extra":false,"source":`, 1),
		"null source":                 strings.Replace(valid, string(mustMarshalFloat64(t, string(generated.Source))), "null", 1),
		"empty source":                strings.Replace(valid, string(mustMarshalFloat64(t, string(generated.Source))), `""`, 1),
		"bad binding":                 strings.Replace(valid, "app.Compare", "other.Compare", 1),
		"duplicate binding name":      strings.Replace(valid, `"name":`, `"name":"wrong","name":`, 1),
		"invalid source UTF8":         strings.Replace(valid, `"source":"`, "\"source\":\"\xff", 1),
		"surrogate binding":           strings.Replace(valid, "app.Compare", `\ud800`, 1),
		"trailing":                    valid + "{}",
		"empty":                       "",
	}
	for name, data := range cases {
		t.Run(name, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "emitter")
			script := []byte("#!/bin/sh\ncat <<'CONDITIONAL_RESPONSE_END'\n" + data + "\nCONDITIONAL_RESPONSE_END\n")
			if err := os.WriteFile(path, script, 0700); err != nil {
				t.Fatal(err)
			}
			hash := sha256.Sum256(script)
			emitter := NativeEmitter{Path: path, SHA256: hex.EncodeToString(hash[:])}
			if generated, err := emitter.GenerateWithProfile(context.Background(), program, Float64ConditionalCapability); err == nil || len(generated.Source) != 0 {
				t.Fatal("bad response returned source or fallback")
			}
		})
	}
	selected.SHA256 = strings.Repeat("0", 64)
	if generated, err := selected.GenerateWithProfile(context.Background(), program, Float64ConditionalCapability); err == nil || len(generated.Source) != 0 {
		t.Fatal("bad checksum returned source")
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
		generated, err := selected.GenerateWithProfile(ctx, program, Float64ConditionalCapability)
		cancel()
		if !errors.Is(err, expected) || len(generated.Source) != 0 {
			t.Fatalf("context refusal differed: %v", err)
		}
	}
}

func TestFloat64ConditionalNativeMatchesReferenceGeneration(t *testing.T) {
	emitter := selectedFloat64Emitter(t)
	program := float64ConditionalProgram()
	expected, err := GenerateWithProfile(program, Float64ConditionalCapability)
	if err != nil {
		t.Fatal(err)
	}
	actual, err := emitter.GenerateWithProfile(context.Background(), program, Float64ConditionalCapability)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(actual.Source, expected.Source) || len(actual.Functions) != len(expected.Functions) {
		t.Fatal("conditional native output differs from the reference generation")
	}
	for index, binding := range expected.Functions {
		actualBinding := actual.Functions[index]
		if binding.Name != actualBinding.Name || binding.Identity.PackageID != actualBinding.Identity.PackageID || binding.Identity.Path != actualBinding.Identity.Path {
			t.Fatal("conditional native output changed a semantic binding")
		}
	}
}
