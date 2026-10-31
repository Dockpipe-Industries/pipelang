package cppbackend

import (
	"context"
	"encoding/json"
	"math"
	"strings"
	"testing"

	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang/coreir"
)

func float64Program() coreir.Program {
	floating := coreir.BinaryFloat(64)
	boolean := coreir.Type{Kind: coreir.TypePrimitive, Primitive: coreir.PrimitiveBool}
	first, second := 0, 1
	left := coreir.Expr{Kind: coreir.ExprReference, Type: floating, Parameter: &first}
	right := coreir.Expr{Kind: coreir.ExprReference, Type: floating, Parameter: &second}
	return coreir.Program{LanguageContract: coreir.LanguageContractV1130, CompilerContract: coreir.CompilerContractV1,
		Functions: []coreir.Function{{
			Identity: coreir.SemanticIdentity{PackageID: "test.float64", Path: "app.Compare", Callable: &coreir.CallableIdentity{
				Parameters: []coreir.SemanticType{{Kind: coreir.TypePrimitive, Primitive: coreir.PrimitiveFloat}, {Kind: coreir.TypePrimitive, Primitive: coreir.PrimitiveFloat}},
				Returns:    coreir.SemanticType{Kind: coreir.TypePrimitive, Primitive: coreir.PrimitiveBool},
			}}, Name: "Compare", ReturnType: boolean,
			Parameters: []coreir.Parameter{{Position: 0, Name: "left", Type: floating}, {Position: 1, Name: "right", Type: floating}},
			Body:       coreir.Expr{Kind: coreir.ExprBinary, Type: boolean, Binary: &coreir.Binary{Operator: coreir.OperatorLessOrEqual, Left: &left, Right: &right}},
		}}}
}

func TestFloat64ProfileIsExplicitAndClosed(t *testing.T) {
	base := float64Program()
	if _, err := GenerateWithProfile(base, Float64ComparisonCapability); err != nil {
		t.Fatal(err)
	}
	if source, err := Generate(base); err == nil || len(source) != 0 {
		t.Fatal("pilot admitted float64")
	}
	for _, profile := range []string{"", "unknown", "pipelang.cpp.float64-comparison.v2"} {
		if err := ValidateProfile(base, profile); err == nil {
			t.Fatal("unknown profile validated")
		}
		if generated, err := GenerateWithProfile(base, profile); err == nil || len(generated.Source) != 0 {
			t.Fatal("unknown profile generated")
		}
		if _, err := (NativeEmitter{}).GenerateWithProfile(context.Background(), base, profile); err == nil {
			t.Fatal("unknown native profile admitted")
		}
	}
	cases := map[string]func(*coreir.Program){
		"language": func(program *coreir.Program) {
			program.LanguageContract = coreir.LanguageContractV010
		},
		"compiler": func(program *coreir.Program) {
			program.CompilerContract = "unknown"
		},
		"empty": func(program *coreir.Program) {
			program.Functions = nil
		},
		"too many functions": func(program *coreir.Program) {
			program.Functions = make([]coreir.Function, 129)
		},
		"duplicate": func(program *coreir.Program) {
			program.Functions = append(program.Functions, program.Functions[0])
		},
		"missing callable": func(program *coreir.Program) {
			program.Functions[0].Identity.Callable = nil
		},
		"wrong callable": func(program *coreir.Program) {
			program.Functions[0].Identity.Callable.Parameters[0].Primitive = coreir.PrimitiveBool
		},
		"nul identity": func(program *coreir.Program) {
			program.Functions[0].Identity.Path = "bad\x00path"
		},
		"large identity": func(program *coreir.Program) {
			program.Functions[0].Identity.Path = strings.Repeat("x", 4097)
		},
		"float result": func(program *coreir.Program) {
			program.Functions[0].ReturnType = coreir.BinaryFloat(64)
		},
		"float32": func(program *coreir.Program) {
			program.Functions[0].Parameters[0].Type = coreir.BinaryFloat(32)
		},
		"signed float": func(program *coreir.Program) {
			program.Functions[0].Parameters[0].Type.Numeric.Signed = true
		},
		"source float": func(program *coreir.Program) {
			program.Functions[0].Parameters[0].Type = coreir.Type{Kind: coreir.TypePrimitive, Primitive: coreir.PrimitiveFloat}
		},
		"inactive type": func(program *coreir.Program) {
			program.Functions[0].Parameters[0].Type.Name = "hidden"
		},
		"parameter position": func(program *coreir.Program) {
			program.Functions[0].Parameters[0].Position = 1
		},
		"many parameters": func(program *coreir.Program) {
			program.Functions[0].Parameters = make([]coreir.Parameter, 65)
		},
		"inactive expression": func(program *coreir.Program) {
			program.Functions[0].Body.Call = &coreir.Call{}
		},
		"arithmetic": func(program *coreir.Program) {
			program.Functions[0].Body.Binary.Operator = coreir.OperatorAdd
		},
		"missing operand": func(program *coreir.Program) {
			program.Functions[0].Body.Binary.Left = nil
		},
		"reference": func(program *coreir.Program) {
			*program.Functions[0].Body.Binary.Left.Parameter = 100
		},
		"wrong reference type": func(program *coreir.Program) {
			program.Functions[0].Body.Binary.Left.Type = program.Functions[0].ReturnType
		},
		"nan literal": func(program *coreir.Program) {
			program.Functions[0].Body.Binary.Left = &coreir.Expr{Kind: coreir.ExprLiteral, Type: coreir.BinaryFloat(64), Literal: &coreir.Literal{Float: math.NaN()}}
		},
		"infinite literal": func(program *coreir.Program) {
			program.Functions[0].Body.Binary.Left = &coreir.Expr{Kind: coreir.ExprLiteral, Type: coreir.BinaryFloat(64), Literal: &coreir.Literal{Float: math.Inf(1)}}
		},
		"inactive literal": func(program *coreir.Program) {
			program.Functions[0].Body.Binary.Left = &coreir.Expr{Kind: coreir.ExprLiteral, Type: coreir.BinaryFloat(64), Literal: &coreir.Literal{Float: 1, Bool: true}}
		},
		"cycle": func(program *coreir.Program) {
			body := &program.Functions[0].Body
			*body = coreir.Expr{Kind: coreir.ExprUnary, Type: body.Type, Unary: &coreir.Unary{Operator: coreir.OperatorNot, Operand: body}}
		},
		"depth": func(program *coreir.Program) {
			for index := 0; index < 64; index++ {
				previous := program.Functions[0].Body
				program.Functions[0].Body = coreir.Expr{Kind: coreir.ExprUnary, Type: previous.Type, Unary: &coreir.Unary{Operator: coreir.OperatorNot, Operand: &previous}}
			}
		},
		"nodes": func(program *coreir.Program) {
			for index := 0; index < 12; index++ {
				previous := program.Functions[0].Body
				program.Functions[0].Body = coreir.Expr{Kind: coreir.ExprBinary, Type: previous.Type, Binary: &coreir.Binary{Operator: coreir.OperatorAnd, Left: &previous, Right: &previous}}
			}
		},
	}
	for name, mutate := range cases {
		t.Run(name, func(t *testing.T) {
			program := float64Program()
			mutate(&program)
			if generated, err := GenerateWithProfile(program, Float64ComparisonCapability); err == nil || len(generated.Source) != 0 {
				t.Fatal("unsupported Core generated output")
			}
		})
	}
}

func TestFloat64LiteralWireUsesExactBits(t *testing.T) {
	for _, bits := range []uint64{0, 0x8000000000000000, 1, 0x8000000000000001, 0x0010000000000000, 0x7fefffffffffffff, 0xffefffffffffffff, 0x3fb999999999999a} {
		program := float64Program()
		program.Functions[0].Body.Binary.Right = &coreir.Expr{Kind: coreir.ExprLiteral, Type: coreir.BinaryFloat(64), Literal: &coreir.Literal{Float: math.Float64frombits(bits)}}
		request, err := projectFloat64Program(program)
		if err != nil {
			t.Fatal(err)
		}
		encoded, err := json.Marshal(request)
		if err != nil {
			t.Fatal(err)
		}
		if strings.Contains(string(encoded), `"float":`) || !strings.Contains(string(encoded), `"float64_bits":`) {
			t.Fatal("binary64 literal passed through decimal JSON")
		}
		projected := request.Functions[0].Body.Operands[1]
		if projected.Float64Bits == "" || len(projected.Float64Bits) != 16 {
			t.Fatal("missing exact bits")
		}
	}
}
