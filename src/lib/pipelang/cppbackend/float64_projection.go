package cppbackend

import (
	"fmt"
	"math"
	"reflect"
	"sort"
	"strings"
	"unicode/utf8"

	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang/coreir"
)

const float64RequestContract = "pipelang.cpp-emitter.request.v2"
const float64ResponseContract = "pipelang.cpp-emitter.response.v2"
const float64ExpressionLimit = 4096
const float64DepthLimit = 64

type float64Request struct {
	Contract         string            `json:"contract"`
	Profile          string            `json:"profile"`
	LanguageContract string            `json:"language_contract"`
	CompilerContract string            `json:"compiler_contract"`
	Functions        []float64Function `json:"functions"`
}

type float64Function struct {
	PackageID  string            `json:"package_id"`
	Path       string            `json:"path"`
	Parameters []string          `json:"parameters"`
	Body       float64Expression `json:"body"`
}

type float64Expression struct {
	Kind        string              `json:"kind"`
	Type        string              `json:"type"`
	Parameter   *int                `json:"parameter,omitempty"`
	Boolean     *bool               `json:"boolean,omitempty"`
	Float64Bits string              `json:"float64_bits,omitempty"`
	Operator    string              `json:"operator,omitempty"`
	Operands    []float64Expression `json:"operands,omitempty"`
}

func float64TypeTag(typ coreir.Type) (string, error) {
	if reflect.DeepEqual(typ, coreir.BinaryFloat(64)) {
		return "float64", nil
	}
	if reflect.DeepEqual(typ, coreir.Type{Kind: coreir.TypePrimitive, Primitive: coreir.PrimitiveBool}) {
		return "bool", nil
	}
	return "", fmt.Errorf("float64 comparison profile requires exact binary64 or bool types")
}

func validFloat64Identity(value string) bool {
	return value != "" && len(value) <= 4096 && utf8.ValidString(value) && !strings.ContainsRune(value, '\x00')
}

func projectFloat64Program(program coreir.Program) (float64Request, error) {
	if program.LanguageContract != coreir.LanguageContractV1130 || program.CompilerContract != coreir.CompilerContractV1 || len(program.Functions) == 0 || len(program.Functions) > 128 {
		return float64Request{}, fmt.Errorf("float64 comparison profile requires v0.113.0/compiler.v1 and 1..128 functions")
	}
	request := float64Request{Contract: float64RequestContract, Profile: Float64ComparisonCapability,
		LanguageContract: program.LanguageContract, CompilerContract: program.CompilerContract,
		Functions: make([]float64Function, 0, len(program.Functions))}
	identities := make(map[string]bool)
	nodes := 0
	for _, function := range program.Functions {
		identity := function.Identity
		if !validFloat64Identity(identity.PackageID) || !validFloat64Identity(identity.Path) || !validFloat64Identity(function.Name) || identities[key(identity)] {
			return float64Request{}, fmt.Errorf("float64 comparison function has invalid or duplicate identity")
		}
		identities[key(identity)] = true
		returnType, err := float64TypeTag(function.ReturnType)
		if err != nil || returnType != "bool" || len(function.Parameters) > 64 {
			return float64Request{}, fmt.Errorf("float64 comparison functions return bool and accept at most 64 parameters")
		}
		projected := float64Function{PackageID: identity.PackageID, Path: identity.Path, Parameters: make([]string, 0, len(function.Parameters))}
		if identity.Callable == nil || len(identity.Callable.Parameters) != len(function.Parameters) ||
			!reflect.DeepEqual(identity.Callable.Returns, coreir.SemanticType{Kind: coreir.TypePrimitive, Primitive: coreir.PrimitiveBool}) {
			return float64Request{}, fmt.Errorf("float64 comparison callable identity does not match its signature")
		}
		for position, parameter := range function.Parameters {
			tag, err := float64TypeTag(parameter.Type)
			if err != nil || parameter.Position != position || !validFloat64Identity(parameter.Name) {
				return float64Request{}, fmt.Errorf("float64 comparison parameter has invalid type, name or position")
			}
			primitive := coreir.PrimitiveBool
			if tag == "float64" {
				primitive = coreir.PrimitiveFloat
			}
			if !reflect.DeepEqual(identity.Callable.Parameters[position], coreir.SemanticType{Kind: coreir.TypePrimitive, Primitive: primitive}) {
				return float64Request{}, fmt.Errorf("float64 comparison parameter identity differs from its representation")
			}
			projected.Parameters = append(projected.Parameters, tag)
		}
		projected.Body, err = projectFloat64Expression(&function.Body, projected.Parameters, 1, &nodes)
		if err != nil {
			return float64Request{}, err
		}
		if projected.Body.Type != "bool" {
			return float64Request{}, fmt.Errorf("float64 comparison body must return bool")
		}
		request.Functions = append(request.Functions, projected)
	}
	// The closed, bounded projection above rejects cycles and inactive payloads
	// before calling the authoritative recursive Core checker.
	if err := coreir.ValidateProgram(program); err != nil {
		return float64Request{}, err
	}
	sort.Slice(request.Functions, func(left, right int) bool {
		first := request.Functions[left]
		second := request.Functions[right]
		return first.PackageID+"\x00"+first.Path < second.PackageID+"\x00"+second.Path
	})
	return request, nil
}

func projectFloat64Expression(expression *coreir.Expr, parameters []string, depth int, nodes *int) (float64Expression, error) {
	*nodes = *nodes + 1
	if expression == nil || depth > float64DepthLimit || *nodes > float64ExpressionLimit {
		return float64Expression{}, fmt.Errorf("float64 comparison expression is missing, cyclic or exceeds its resource bounds")
	}
	tag, err := float64TypeTag(expression.Type)
	if err != nil {
		return float64Expression{}, err
	}
	result := float64Expression{Kind: string(expression.Kind), Type: tag}
	active := coreir.Expr{Kind: expression.Kind, Type: expression.Type}
	switch expression.Kind {
	case coreir.ExprReference:
		active.Parameter = expression.Parameter
		if expression.Parameter == nil || *expression.Parameter < 0 || *expression.Parameter >= len(parameters) || parameters[*expression.Parameter] != tag {
			return float64Expression{}, fmt.Errorf("float64 comparison reference has invalid position or type")
		}
		position := *expression.Parameter
		result.Parameter = &position
	case coreir.ExprLiteral:
		active.Literal = expression.Literal
		if expression.Literal == nil {
			return float64Expression{}, fmt.Errorf("float64 comparison literal is missing")
		}
		literal := expression.Literal
		if literal.String != "" || literal.Int != 0 {
			return float64Expression{}, fmt.Errorf("float64 comparison literal carries an inactive payload")
		}
		if tag == "float64" {
			if literal.Bool || math.IsInf(literal.Float, 0) || math.IsNaN(literal.Float) {
				return float64Expression{}, fmt.Errorf("float64 comparison literal must be finite and exclusive")
			}
			result.Float64Bits = fmt.Sprintf("%016x", math.Float64bits(literal.Float))
		} else {
			if math.Float64bits(literal.Float) != 0 {
				return float64Expression{}, fmt.Errorf("bool literal carries a numeric payload")
			}
			boolean := literal.Bool
			result.Boolean = &boolean
		}
	case coreir.ExprUnary:
		active.Unary = expression.Unary
		if expression.Unary == nil || expression.Unary.Operator != coreir.OperatorNot || tag != "bool" {
			return float64Expression{}, fmt.Errorf("float64 comparison profile admits only logical not")
		}
		operand, err := projectFloat64Expression(expression.Unary.Operand, parameters, depth+1, nodes)
		if err != nil {
			return float64Expression{}, err
		}
		if operand.Type != "bool" {
			return float64Expression{}, fmt.Errorf("logical not requires bool")
		}
		result.Operator = string(coreir.OperatorNot)
		result.Operands = []float64Expression{operand}
	case coreir.ExprBinary:
		active.Binary = expression.Binary
		if expression.Binary == nil || tag != "bool" {
			return float64Expression{}, fmt.Errorf("float64 comparison binary expression is invalid")
		}
		left, err := projectFloat64Expression(expression.Binary.Left, parameters, depth+1, nodes)
		if err != nil {
			return float64Expression{}, err
		}
		right, err := projectFloat64Expression(expression.Binary.Right, parameters, depth+1, nodes)
		if err != nil {
			return float64Expression{}, err
		}
		operator := expression.Binary.Operator
		if left.Type != right.Type || !float64OperatorAllowed(operator, left.Type) {
			return float64Expression{}, fmt.Errorf("float64 comparison has unsupported operator or mismatched operands")
		}
		result.Operator = string(operator)
		result.Operands = []float64Expression{left, right}
	default:
		return float64Expression{}, fmt.Errorf("unsupported float64 comparison operation %q", expression.Kind)
	}
	if !reflect.DeepEqual(*expression, active) {
		return float64Expression{}, fmt.Errorf("float64 comparison expression carries an inactive payload")
	}
	return result, nil
}

func float64OperatorAllowed(operator coreir.Operator, operandType string) bool {
	switch operator {
	case coreir.OperatorAnd, coreir.OperatorOr:
		return operandType == "bool"
	case coreir.OperatorEqual, coreir.OperatorNotEqual:
		return operandType == "bool" || operandType == "float64"
	case coreir.OperatorLessThan, coreir.OperatorLessOrEqual, coreir.OperatorGreaterThan, coreir.OperatorGreaterOrEqual:
		return operandType == "float64"
	default:
		return false
	}
}
