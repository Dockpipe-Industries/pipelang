package cppbackend

import (
	"fmt"
	"strings"
)

// validateFloat64CallGraph bounds both syntax and transitive evaluation work.
// Core subsequently checks complete source semantic identities, including names.
func validateFloat64CallGraph(request float64Request) error {
	functions := make(map[string]float64Function, len(request.Functions))
	for _, function := range request.Functions {
		functions[function.PackageID+"\x00"+function.Path] = function
	}
	type summary struct {
		work  int
		depth int
	}
	state := make(map[string]uint8)
	summaries := make(map[string]summary)
	var visit func(string) (summary, error)
	var expressionWork func(float64Expression, float64Function) (summary, error)
	expressionWork = func(expression float64Expression, owner float64Function) (summary, error) {
		result := summary{work: 1, depth: 1}
		children := expression.Operands
		if expression.Kind == "call" {
			targetKey := expression.Callee.PackageID + "\x00" + expression.Callee.Path
			target, exists := functions[targetKey]
			arguments := *expression.Arguments
			if !exists || len(arguments) != len(target.Parameters) || expression.Type != target.ReturnType {
				return summary{}, fmt.Errorf("typed call target signature mismatch")
			}
			if target.PackageID != owner.PackageID || float64OwnerPath(target.Path) != float64OwnerPath(owner.Path) {
				return summary{}, fmt.Errorf("typed call target owner mismatch")
			}
			for index, argument := range arguments {
				if argument.Type != target.Parameters[index] {
					return summary{}, fmt.Errorf("typed call argument type mismatch")
				}
			}
			targetSummary, err := visit(targetKey)
			if err != nil {
				return summary{}, err
			}
			result.work += targetSummary.work
			result.depth = targetSummary.depth + 1
			children = arguments
		}
		for _, child := range children {
			childSummary, err := expressionWork(child, owner)
			if err != nil {
				return summary{}, err
			}
			result.work += childSummary.work
			if childSummary.depth > result.depth {
				result.depth = childSummary.depth
			}
			if result.work > float64ExpressionLimit || result.depth > float64DepthLimit {
				return summary{}, fmt.Errorf("typed call expanded evaluation resource bound")
			}
		}
		if result.work > float64ExpressionLimit || result.depth > float64DepthLimit {
			return summary{}, fmt.Errorf("typed call expanded evaluation resource bound")
		}
		return result, nil
	}
	visit = func(key string) (summary, error) {
		if state[key] == 1 {
			return summary{}, fmt.Errorf("typed call graph contains recursive cycle")
		}
		if state[key] == 2 {
			return summaries[key], nil
		}
		state[key] = 1
		function := functions[key]
		result, err := expressionWork(function.Body, function)
		if err != nil {
			return summary{}, err
		}
		summaries[key] = result
		state[key] = 2
		return result, nil
	}
	for _, function := range request.Functions {
		if _, err := visit(function.PackageID + "\x00" + function.Path); err != nil {
			return err
		}
	}
	return nil
}

func float64OwnerPath(path string) string {
	separator := strings.LastIndex(path, ".")
	if separator < 0 {
		return path
	}
	return path[:separator]
}
