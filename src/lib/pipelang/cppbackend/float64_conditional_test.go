package cppbackend

import (
	"encoding/json"
	"math"
	"strings"
	"testing"

	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang/coreir"
)

func float64ConditionalProgram() coreir.Program {
	program := float64Program()
	function := &program.Functions[0]
	floating := coreir.BinaryFloat(64)
	comparison := function.Body
	comparison.Binary.Left, comparison.Binary.Right = comparison.Binary.Right, comparison.Binary.Left
	comparison.Binary.Operator = coreir.OperatorLessThan
	function.ReturnType = floating
	function.Identity.Callable.Returns = coreir.SemanticType{Kind: coreir.TypePrimitive, Primitive: coreir.PrimitiveFloat}
	function.Body = coreir.Expr{
		Kind: coreir.ExprConditional,
		Type: floating,
		Conditional: &coreir.Conditional{
			Condition: &comparison,
			WhenTrue:  comparison.Binary.Left,
			WhenFalse: comparison.Binary.Right,
		},
	}
	return program
}

func TestFloat64ConditionalProfileSelectionAndReturns(t *testing.T) {
	program := float64ConditionalProgram()
	if err := ValidateProfile(program, Float64ConditionalCapability); err != nil {
		t.Fatal(err)
	}
	generated, err := GenerateWithProfile(program, Float64ConditionalCapability)
	if err != nil || !strings.Contains(string(generated.Source), "inline double ") || len(generated.Functions) != 1 {
		t.Fatalf("binary64 conditional was not generated: %v", err)
	}
	for _, profile := range []string{Capability, Float64ComparisonCapability, "", "unknown", "pipelang.cpp.float64-conditional.v2"} {
		if generated, err := GenerateWithProfile(program, profile); err == nil || len(generated.Source) != 0 {
			t.Fatalf("unsupported profile %q generated source", profile)
		}
	}
	for _, name := range []string{"parameter", "literal", "bool comparison", "bool conditional"} {
		t.Run(name, func(t *testing.T) {
			program := float64ConditionalProgram()
			function := &program.Functions[0]
			switch name {
			case "parameter":
				function.Body = *function.Body.Conditional.WhenFalse
			case "literal":
				function.Body = coreir.Expr{Kind: coreir.ExprLiteral, Type: function.ReturnType, Literal: &coreir.Literal{Float: math.Copysign(0, -1)}}
			case "bool comparison":
				program = float64Program()
			case "bool conditional":
				boolean := coreir.Type{Kind: coreir.TypePrimitive, Primitive: coreir.PrimitiveBool}
				function.ReturnType = boolean
				function.Identity.Callable.Returns = coreir.SemanticType{Kind: coreir.TypePrimitive, Primitive: coreir.PrimitiveBool}
				function.Body.Type = boolean
				function.Body.Conditional.WhenTrue = &coreir.Expr{Kind: coreir.ExprLiteral, Type: boolean, Literal: &coreir.Literal{Bool: true}}
				function.Body.Conditional.WhenFalse = &coreir.Expr{Kind: coreir.ExprLiteral, Type: boolean, Literal: &coreir.Literal{Bool: false}}
			}
			if _, err := GenerateWithProfile(program, Float64ConditionalCapability); err != nil {
				t.Fatal(err)
			}
		})
	}
}

func TestFloat64ConditionalClosedCoreRefusals(t *testing.T) {
	cases := map[string]func(*coreir.Program){
		"return representation": func(program *coreir.Program) {
			program.Functions[0].ReturnType = coreir.Type{Kind: coreir.TypePrimitive, Primitive: coreir.PrimitiveFloat}
		},
		"callable return mismatch": func(program *coreir.Program) {
			program.Functions[0].Identity.Callable.Returns.Primitive = coreir.PrimitiveBool
		},
		"body return mismatch": func(program *coreir.Program) {
			program.Functions[0].Body = *program.Functions[0].Body.Conditional.Condition
		},
		"missing conditional": func(program *coreir.Program) {
			program.Functions[0].Body.Conditional = nil
		},
		"missing condition": func(program *coreir.Program) {
			program.Functions[0].Body.Conditional.Condition = nil
		},
		"missing true arm": func(program *coreir.Program) {
			program.Functions[0].Body.Conditional.WhenTrue = nil
		},
		"missing false arm": func(program *coreir.Program) {
			program.Functions[0].Body.Conditional.WhenFalse = nil
		},
		"numeric condition": func(program *coreir.Program) {
			conditional := program.Functions[0].Body.Conditional
			conditional.Condition = conditional.WhenTrue
		},
		"arm type mismatch": func(program *coreir.Program) {
			conditional := program.Functions[0].Body.Conditional
			conditional.WhenTrue = conditional.Condition
		},
		"result type mismatch": func(program *coreir.Program) {
			program.Functions[0].Body.Type = program.Functions[0].Body.Conditional.Condition.Type
		},
		"terminal statement": func(program *coreir.Program) {
			program.Functions[0].Body.Conditional.TerminalStatement = true
		},
		"inactive expression payload": func(program *coreir.Program) {
			program.Functions[0].Body.Call = &coreir.Call{}
		},
		"inactive arm payload": func(program *coreir.Program) {
			program.Functions[0].Body.Conditional.WhenTrue.Literal = &coreir.Literal{Float: 1}
		},
		"cycle": func(program *coreir.Program) {
			body := &program.Functions[0].Body
			body.Conditional.WhenTrue = body
		},
		"depth": func(program *coreir.Program) {
			for index := 0; index < 64; index++ {
				previous := program.Functions[0].Body
				program.Functions[0].Body = coreir.Expr{
					Kind: coreir.ExprConditional,
					Type: previous.Type,
					Conditional: &coreir.Conditional{
						Condition: previous.Conditional.Condition,
						WhenTrue:  &previous,
						WhenFalse: previous.Conditional.WhenFalse,
					},
				}
			}
		},
		"nodes": func(program *coreir.Program) {
			for index := 0; index < 12; index++ {
				previous := program.Functions[0].Body
				program.Functions[0].Body = coreir.Expr{
					Kind: coreir.ExprConditional,
					Type: previous.Type,
					Conditional: &coreir.Conditional{
						Condition: previous.Conditional.Condition,
						WhenTrue:  &previous,
						WhenFalse: &previous,
					},
				}
			}
		},
		"arithmetic": func(program *coreir.Program) {
			function := &program.Functions[0]
			conditional := function.Body.Conditional
			function.Body = coreir.Expr{Kind: coreir.ExprBinary, Type: function.ReturnType, Binary: &coreir.Binary{
				Operator: coreir.OperatorAdd, Left: conditional.WhenTrue, Right: conditional.WhenFalse,
			}}
		},
		"numeric negation": func(program *coreir.Program) {
			function := &program.Functions[0]
			operand := function.Body.Conditional.WhenTrue
			function.Body = coreir.Expr{Kind: coreir.ExprUnary, Type: function.ReturnType, Unary: &coreir.Unary{
				Operator: coreir.Operator("negate"), Operand: operand,
			}}
		},
		"call": func(program *coreir.Program) {
			function := &program.Functions[0]
			function.Body = coreir.Expr{Kind: coreir.ExprCall, Type: function.ReturnType, Call: &coreir.Call{}}
		},
		"block": func(program *coreir.Program) {
			function := &program.Functions[0]
			function.Body = coreir.Expr{Kind: coreir.ExprBlock, Type: function.ReturnType}
		},
		"local": func(program *coreir.Program) {
			function := &program.Functions[0]
			function.Body = coreir.Expr{Kind: coreir.ExprImmutableLocal, Type: function.ReturnType, ImmutableLocal: &coreir.ImmutableLocal{}}
		},
		"nonfinite literal": func(program *coreir.Program) {
			function := &program.Functions[0]
			function.Body.Conditional.WhenTrue = &coreir.Expr{Kind: coreir.ExprLiteral, Type: function.ReturnType, Literal: &coreir.Literal{Float: math.Inf(1)}}
		},
	}
	for name, mutate := range cases {
		t.Run(name, func(t *testing.T) {
			program := float64ConditionalProgram()
			mutate(&program)
			if err := ValidateProfile(program, Float64ConditionalCapability); err == nil {
				t.Fatal("invalid Core validated")
			}
			if generated, err := GenerateWithProfile(program, Float64ConditionalCapability); err == nil || len(generated.Source) != 0 {
				t.Fatal("invalid Core generated source")
			}
		})
	}
}

func TestFloat64ConditionalWireShape(t *testing.T) {
	program := float64ConditionalProgram()
	request, err := projectFloat64ProgramWithProfile(program, Float64ConditionalCapability)
	if err != nil {
		t.Fatal(err)
	}
	if request.Contract != "pipelang.cpp-emitter.request.v3" || request.Profile != Float64ConditionalCapability || request.Functions[0].ReturnType != "float64" {
		t.Fatal("conditional request did not select its v3 contract and return type")
	}
	encoded := mustMarshalFloat64(t, request)
	var object struct {
		Functions []map[string]json.RawMessage `json:"functions"`
	}
	if err := json.Unmarshal(encoded, &object); err != nil {
		t.Fatal(err)
	}
	function := object.Functions[0]
	if len(function) != 5 || string(function["return_type"]) != `"float64"` {
		t.Fatalf("unexpected v3 function members: %s", encoded)
	}
	var body map[string]json.RawMessage
	if err := json.Unmarshal(function["body"], &body); err != nil {
		t.Fatal(err)
	}
	if len(body) != 3 || string(body["kind"]) != `"conditional"` || string(body["type"]) != `"float64"` {
		t.Fatalf("unexpected conditional members: %s", function["body"])
	}
	var operands []float64Expression
	if err := json.Unmarshal(body["operands"], &operands); err != nil {
		t.Fatal(err)
	}
	if len(operands) != 3 || operands[0].Type != "bool" || *operands[1].Parameter != 1 || *operands[2].Parameter != 0 {
		t.Fatal("condition or ordered arms changed during projection")
	}
	previous, err := projectFloat64Program(float64Program())
	if err != nil {
		t.Fatal(err)
	}
	if previous.Contract != "pipelang.cpp-emitter.request.v2" || strings.Contains(string(mustMarshalFloat64(t, previous)), `"return_type"`) {
		t.Fatal("comparison v2 wire shape changed")
	}
}
