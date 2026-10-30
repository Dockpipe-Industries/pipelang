package cppbackend

import (
	"context"
	"fmt"

	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang/coreir"
)

// Float64ComparisonCapability is an explicit comparison-only backend profile.
// It preserves the existing binary64 host domain; only literals must be finite.
const Float64ComparisonCapability = "pipelang.cpp.float64-comparison.v1"

// ValidateProfile checks the complete selected capability before generation.
// Existing Validate remains the unchanged pilot admission boundary.
func ValidateProfile(program coreir.Program, profile string) error {
	switch profile {
	case Capability:
		return Validate(program)
	case Float64ComparisonCapability:
		_, err := projectFloat64Program(program)
		return err
	default:
		return fmt.Errorf("unsupported C++ profile %q", profile)
	}
}

// GenerateWithProfile selects a capability explicitly. No profile is inferred
// from source types, environment variables, failed emission or tool discovery.
func GenerateWithProfile(program coreir.Program, profile string) (Generated, error) {
	switch profile {
	case Capability:
		return GenerateWithNames(program)
	case Float64ComparisonCapability:
		request, err := projectFloat64Program(program)
		if err != nil {
			return Generated{}, err
		}
		return generateFloat64(request, program), nil
	default:
		return Generated{}, fmt.Errorf("unsupported C++ profile %q", profile)
	}
}

// GenerateWithProfile retains the selected executable identity and shared
// process boundary while choosing the corresponding private wire contract.
func (emitter NativeEmitter) GenerateWithProfile(ctx context.Context, program coreir.Program, profile string) (Generated, error) {
	switch profile {
	case Capability:
		return emitter.Generate(ctx, program)
	case Float64ComparisonCapability:
		return emitter.generateFloat64(ctx, program)
	default:
		return Generated{}, fmt.Errorf("unsupported C++ profile %q", profile)
	}
}
