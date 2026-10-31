package cppbackend

import (
	"context"
	"fmt"

	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang/coreir"
)

// Float64ComparisonCapability is an explicit comparison-only backend profile.
// It preserves the existing binary64 host domain; only literals must be finite.
const Float64ComparisonCapability = "pipelang.cpp.float64-comparison.v1"

// Float64ConditionalCapability additionally admits typed conditional values and returns.
const Float64ConditionalCapability = "pipelang.cpp.float64-conditional.v1"

// Float64CallsCapability admits bounded typed intra-program calls through the native emitter.
const Float64CallsCapability = "pipelang.cpp.float64-calls.v1"

// ValidateProfile checks the complete selected capability before generation.
// Existing Validate remains the unchanged pilot admission boundary.
func ValidateProfile(program coreir.Program, profile string) error {
	switch profile {
	case Capability:
		return Validate(program)
	case Float64ComparisonCapability, Float64ConditionalCapability, Float64CallsCapability:
		_, err := projectFloat64ProgramWithProfile(program, profile)
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
	case Float64ComparisonCapability, Float64ConditionalCapability, Float64CallsCapability:
		request, err := projectFloat64ProgramWithProfile(program, profile)
		if err != nil {
			return Generated{}, err
		}
		if profile == Float64CallsCapability {
			return Generated{}, fmt.Errorf("profile %s requires an explicitly selected native C++ emitter", profile)
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
	case Float64ComparisonCapability, Float64ConditionalCapability, Float64CallsCapability:
		return emitter.generateFloat64WithProfile(ctx, program, profile)
	default:
		return Generated{}, fmt.Errorf("unsupported C++ profile %q", profile)
	}
}
