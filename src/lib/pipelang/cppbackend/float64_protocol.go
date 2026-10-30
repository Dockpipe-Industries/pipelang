package cppbackend

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"strconv"

	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang/coreir"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang/internal/nativeprocess"
)

// float64Object admits exact keys before typed decoding. encoding/json alone
// permits duplicate keys, case aliases, missing fields and null scalar defaults.
func float64Object(data []byte, names ...string) (map[string]json.RawMessage, error) {
	decoder := json.NewDecoder(bytes.NewReader(data))
	opening, err := decoder.Token()
	if err != nil || opening != json.Delim('{') {
		return nil, errors.New("expected a response object")
	}
	allowed := make(map[string]bool, len(names))
	for _, name := range names {
		allowed[name] = true
	}
	fields := make(map[string]json.RawMessage, len(names))
	for decoder.More() {
		key, err := decoder.Token()
		if err != nil {
			return nil, err
		}
		name, ok := key.(string)
		if !ok || !allowed[name] {
			return nil, fmt.Errorf("unsupported response field %q", name)
		}
		if _, exists := fields[name]; exists {
			return nil, fmt.Errorf("duplicate response field %q", name)
		}
		var value json.RawMessage
		if err := decoder.Decode(&value); err != nil {
			return nil, err
		}
		fields[name] = value
	}
	if closing, err := decoder.Token(); err != nil || closing != json.Delim('}') {
		return nil, errors.New("response object is not complete")
	}
	var trailing any
	if err := decoder.Decode(&trailing); err != io.EOF {
		return nil, errors.New("response object contains trailing data")
	}
	if len(fields) != len(names) {
		return nil, errors.New("response object is missing required fields")
	}
	return fields, nil
}

func float64String(raw json.RawMessage) (string, error) {
	data := bytes.TrimSpace(raw)
	if len(data) == 0 || data[0] != '"' {
		return "", errors.New("expected a response string")
	}
	var value string
	if err := json.Unmarshal(data, &value); err != nil {
		return "", err
	}
	// Reject unpaired JSON surrogates instead of accepting encoding/json's
	// replacement rune; PipeLang text preserves Unicode scalar values exactly.
	for index := 1; index < len(data)-1; index++ {
		if data[index] != '\\' {
			continue
		}
		index++
		if data[index] != 'u' {
			continue
		}
		code, err := strconv.ParseUint(string(data[index+1:index+5]), 16, 16)
		if err != nil {
			return "", err
		}
		index += 4
		if code >= 0xdc00 && code <= 0xdfff {
			return "", errors.New("unpaired low surrogate in response string")
		}
		if code < 0xd800 || code > 0xdbff {
			continue
		}
		if index+6 >= len(data) || data[index+1] != '\\' || data[index+2] != 'u' {
			return "", errors.New("unpaired high surrogate in response string")
		}
		low, err := strconv.ParseUint(string(data[index+3:index+7]), 16, 16)
		if err != nil || low < 0xdc00 || low > 0xdfff {
			return "", errors.New("invalid surrogate pair in response string")
		}
		index += 6
	}
	return value, nil
}

func (emitter NativeEmitter) generateFloat64(ctx context.Context, program coreir.Program) (Generated, error) {
	return emitter.generateFloat64WithProfile(ctx, program, Float64ComparisonCapability)
}

func (emitter NativeEmitter) generateFloat64WithProfile(ctx context.Context, program coreir.Program, selectedProfile string) (Generated, error) {
	request, err := projectFloat64ProgramWithProfile(program, selectedProfile)
	if err != nil {
		return Generated{}, err
	}
	encoded, err := json.Marshal(request)
	if err != nil || len(encoded) > maximumNativeRequest {
		return Generated{}, errors.New("float64 emitter request exceeds its encoding contract")
	}
	output, err := nativeprocess.Run(ctx, emitter.executable(), encoded, maximumNativeResponse)
	if err != nil {
		return Generated{}, err
	}
	fields, err := float64Object(output, "contract", "profile", "source", "functions")
	if err != nil {
		return Generated{}, err
	}
	contract, contractError := float64String(fields["contract"])
	profile, profileError := float64String(fields["profile"])
	source, sourceError := float64String(fields["source"])
	if err := errors.Join(contractError, profileError, sourceError); err != nil {
		return Generated{}, err
	}
	expectedContract := float64ResponseContract
	if selectedProfile == Float64ConditionalCapability {
		expectedContract = float64ConditionalResponseContract
	}
	if selectedProfile == Float64CallsCapability {
		expectedContract = float64CallsResponseContract
	}
	if contract != expectedContract || profile != selectedProfile || len(source) == 0 || len(source) > maximumNativeSource {
		return Generated{}, errors.New("float64 emitter response has invalid contract, profile or source size")
	}
	rawFunctions := bytes.TrimSpace(fields["functions"])
	if len(rawFunctions) == 0 || rawFunctions[0] != '[' {
		return Generated{}, errors.New("float64 response functions must be an array")
	}
	functionDecoder := json.NewDecoder(bytes.NewReader(rawFunctions))
	if _, err := functionDecoder.Token(); err != nil {
		return Generated{}, err
	}
	functions := make([]json.RawMessage, 0, len(request.Functions))
	for functionDecoder.More() {
		if len(functions) >= len(request.Functions) {
			return Generated{}, errors.New("float64 emitter response exceeded its binding count")
		}
		var raw json.RawMessage
		if err := functionDecoder.Decode(&raw); err != nil {
			return Generated{}, err
		}
		functions = append(functions, raw)
	}
	if closing, err := functionDecoder.Token(); err != nil || closing != json.Delim(']') {
		return Generated{}, errors.New("incomplete float64 response binding array")
	}
	if len(functions) != len(request.Functions) {
		return Generated{}, errors.New("float64 emitter response changed the binding count")
	}
	for index, raw := range functions {
		binding, err := float64Object(raw, "package_id", "path", "name")
		if err != nil {
			return Generated{}, err
		}
		packageID, packageError := float64String(binding["package_id"])
		path, pathError := float64String(binding["path"])
		symbol, symbolError := float64String(binding["name"])
		if err := errors.Join(packageError, pathError, symbolError); err != nil {
			return Generated{}, err
		}
		expected := request.Functions[index]
		if packageID != expected.PackageID || path != expected.Path || symbol != float64Symbol(expected) {
			return Generated{}, errors.New("float64 emitter response changed a semantic binding")
		}
	}
	return Generated{Source: []byte(source), Functions: float64Bindings(program, request.Functions)}, nil
}
