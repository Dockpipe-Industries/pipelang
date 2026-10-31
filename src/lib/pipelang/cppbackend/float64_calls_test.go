package cppbackend

import (
	"bytes"
	"context"
	"encoding/json"
	"os/exec"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang/coreir"
)

func float64CallsProgram() coreir.Program {
	program := float64ConditionalProgram()
	target := program.Functions[0]
	caller := target
	caller.Name = "Caller"
	caller.Identity.Path = "app.Caller"
	arguments := []*coreir.Expr{target.Body.Conditional.WhenFalse, target.Body.Conditional.WhenTrue}
	caller.Body = coreir.Expr{Kind: coreir.ExprCall, Type: target.ReturnType,
		Call: &coreir.Call{Target: target.Identity, TargetName: target.Name, Arguments: arguments}}
	program.Functions = append(program.Functions, caller)
	return program
}

func TestFloat64CallsAdmissionAndNativeOnlySelection(t *testing.T) {
	program := float64CallsProgram()
	if err := ValidateProfile(program, Float64CallsCapability); err != nil {
		t.Fatal(err)
	}
	for _, profile := range []string{Capability, Float64ComparisonCapability, Float64ConditionalCapability} {
		if err := ValidateProfile(program, profile); err == nil {
			t.Fatalf("prior profile %s admitted calls", profile)
		}
	}
	generated, err := GenerateWithProfile(program, Float64CallsCapability)
	if err == nil || !strings.Contains(err.Error(), "native C++ emitter") || len(generated.Source) != 0 {
		t.Fatal("calls gained an implicit Go emitter implementation")
	}
	request, err := projectFloat64ProgramWithProfile(program, Float64CallsCapability)
	if err != nil {
		t.Fatal(err)
	}
	if request.Contract != float64CallsRequestContract || request.Profile != Float64CallsCapability {
		t.Fatal("calls did not select v4")
	}
	cases := map[string]func(*coreir.Program){
		"unknown target": func(program *coreir.Program) { program.Functions[1].Body.Call.Target.Path = "app.Missing" },
		"wrong source identity": func(program *coreir.Program) {
			identity := *program.Functions[1].Body.Call.Target.Callable
			identity.Returns = coreir.SemanticType{Kind: coreir.TypePrimitive, Primitive: coreir.PrimitiveBool}
			program.Functions[1].Body.Call.Target.Callable = &identity
		},
		"wrong target name": func(program *coreir.Program) { program.Functions[1].Body.Call.TargetName = "Other" },
		"missing argument": func(program *coreir.Program) {
			program.Functions[1].Body.Call.Arguments = program.Functions[1].Body.Call.Arguments[:1]
		},
		"null argument": func(program *coreir.Program) { program.Functions[1].Body.Call.Arguments[0] = nil },
		"result mismatch": func(program *coreir.Program) {
			program.Functions[1].Body.Type = coreir.Type{Kind: coreir.TypePrimitive, Primitive: coreir.PrimitiveBool}
		},
		"self recursion": func(program *coreir.Program) {
			call := program.Functions[1].Body.Call
			call.Target = program.Functions[1].Identity
			call.TargetName = program.Functions[1].Name
		},
		"mutual recursion": func(program *coreir.Program) {
			program.Functions[0].Body = program.Functions[1].Body
			call := *program.Functions[1].Body.Call
			call.Target = program.Functions[1].Identity
			call.TargetName = program.Functions[1].Name
			program.Functions[0].Body.Call = &call
		},
		"inactive payload": func(program *coreir.Program) { program.Functions[1].Body.Literal = &coreir.Literal{Float: 1} },
	}
	for name, mutate := range cases {
		t.Run(name, func(t *testing.T) {
			program := float64CallsProgram()
			mutate(&program)
			if err := ValidateProfile(program, Float64CallsCapability); err == nil {
				t.Fatal("invalid call admitted")
			}
		})
	}
}

func TestFloat64CallsExpandedGraphBounds(t *testing.T) {
	for _, count := range []int{14, 65} {
		t.Run(strconv.Itoa(count), func(t *testing.T) {
			program := float64CallsProgram()
			function := program.Functions[0]
			program.Functions = program.Functions[:1]
			for index := 1; index < count; index++ {
				previous := program.Functions[index-1]
				next := function
				next.Name = "Chain" + strings.Repeat("x", index)
				next.Identity.Path = "app." + next.Name
				call := coreir.Expr{Kind: coreir.ExprCall, Type: function.ReturnType,
					Call: &coreir.Call{Target: previous.Identity, TargetName: previous.Name,
						Arguments: []*coreir.Expr{function.Body.Conditional.WhenFalse, function.Body.Conditional.WhenTrue}}}
				next.Body = call
				if count == 14 {
					next.Body = coreir.Expr{Kind: coreir.ExprConditional, Type: function.ReturnType,
						Conditional: &coreir.Conditional{Condition: function.Body.Conditional.Condition,
							WhenTrue: &call, WhenFalse: &call}}
				}
				program.Functions = append(program.Functions, next)
			}
			if err := ValidateProfile(program, Float64CallsCapability); err == nil || !strings.Contains(err.Error(), "resource bound") {
				t.Fatalf("unbounded acyclic calls admitted: %v", err)
			}
		})
	}
}

func TestFloat64CallsNativeWireRefusals(t *testing.T) {
	emitter := selectedFloat64Emitter(t)
	request, err := projectFloat64ProgramWithProfile(float64CallsProgram(), Float64CallsCapability)
	if err != nil {
		t.Fatal(err)
	}
	valid := string(mustMarshalFloat64(t, request))
	cases := map[string]string{
		"v3 envelope":         strings.Replace(valid, float64CallsRequestContract, float64ConditionalRequestContract, 1),
		"wrong profile":       strings.Replace(valid, Float64CallsCapability, Float64ConditionalCapability, 1),
		"unknown target":      strings.Replace(valid, `"callee":{"package_id":"test.float64","path":"app.Compare"}`, `"callee":{"package_id":"test.float64","path":"app.Missing"}`, 1),
		"duplicate callee":    strings.Replace(valid, `"callee":`, `"callee":{},"callee":`, 1),
		"unknown call member": strings.Replace(valid, `"kind":"call",`, `"kind":"call","extra":false,`, 1),
		"null arguments":      strings.Replace(valid, `"arguments":[`, `"arguments":null,"unexpected":[`, 1),
	}
	for name, mutate := range map[string]func(*float64Request){
		"missing target": func(request *float64Request) { request.Functions[0].Body.Callee.Path = "app.Missing" },
		"wrong package":  func(request *float64Request) { request.Functions[0].Body.Callee.PackageID = "other" },
		"wrong arity": func(request *float64Request) {
			arguments := (*request.Functions[0].Body.Arguments)[:1]
			request.Functions[0].Body.Arguments = &arguments
		},
		"wrong argument type": func(request *float64Request) {
			value := true
			(*request.Functions[0].Body.Arguments)[0] = float64Expression{Kind: "literal", Type: "bool", Boolean: &value}
		},
		"wrong result": func(request *float64Request) {
			request.Functions[0].Body.Type = "bool"
			request.Functions[0].ReturnType = "bool"
		},
		"self cycle": func(request *float64Request) { request.Functions[0].Body.Callee.Path = request.Functions[0].Path },
	} {
		var changed float64Request
		if err := json.Unmarshal([]byte(valid), &changed); err != nil {
			t.Fatal(err)
		}
		mutate(&changed)
		cases[name] = string(mustMarshalFloat64(t, changed))
	}
	for name, input := range cases {
		t.Run(name, func(t *testing.T) {
			if input == valid {
				t.Fatal("wire refusal mutation did not change input")
			}
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			defer cancel()
			command := exec.CommandContext(ctx, emitter.Path)
			command.Stdin = strings.NewReader(input)
			var output, diagnostic bytes.Buffer
			command.Stdout = &output
			command.Stderr = &diagnostic
			if err := command.Run(); err == nil || output.Len() != 0 || diagnostic.Len() == 0 {
				t.Fatalf("invalid v4 produced output or success: %v", err)
			}
		})
	}
}
