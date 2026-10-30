package cppbackend

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"reflect"
	"sort"

	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang/coreir"
	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang/internal/nativeprocess"
)

const nativeRequestContract = "pipelang.cpp-emitter.request.v1"
const nativeResponseContract = "pipelang.cpp-emitter.response.v1"
const maximumNativeRequest = 16 << 20
const maximumNativeResponse = 32 << 20
const maximumNativeSource = 16 << 20

// NativeEmitter explicitly selects one checksum-bound experimental C++ emitter.
// Semantic admission remains owned by Validate; this does not change the default backend.
type NativeEmitter struct {
	Path   string
	SHA256 string
}

type nativeRequest struct {
	Contract string         `json:"contract"`
	Program  coreir.Program `json:"program"`
}

type nativeBinding struct {
	Identity coreir.SemanticIdentity `json:"identity"`
	Name     string                  `json:"name"`
}

type nativeResponse struct {
	Contract  string          `json:"contract"`
	Source    string          `json:"source"`
	Functions []nativeBinding `json:"functions"`
}

func (emitter NativeEmitter) executable() nativeprocess.Executable {
	return nativeprocess.Executable{Path: emitter.Path, SHA256: emitter.SHA256, Label: "native emitter"}
}

func (emitter NativeEmitter) verifyIdentity() error {
	return emitter.executable().Verify()
}

// Generate emits through the selected native process after authoritative Core
// validation. It refuses identity, protocol and resource failures without fallback.
func (emitter NativeEmitter) Generate(ctx context.Context, program coreir.Program) (Generated, error) {
	if err := Validate(program); err != nil {
		return Generated{}, err
	}
	request, err := json.Marshal(nativeRequest{Contract: nativeRequestContract, Program: program})
	if err != nil || len(request) > maximumNativeRequest {
		return Generated{}, errors.New("native emitter request exceeds its encoding contract")
	}
	output, err := nativeprocess.Run(ctx, emitter.executable(), request, maximumNativeResponse)
	if err != nil {
		return Generated{}, err
	}
	var response nativeResponse
	decoder := json.NewDecoder(bytes.NewReader(output))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&response); err != nil {
		return Generated{}, fmt.Errorf("decode native emitter response: %w", err)
	}
	var trailing any
	if err := decoder.Decode(&trailing); err != io.EOF {
		return Generated{}, errors.New("native emitter response contains trailing data")
	}
	if response.Contract != nativeResponseContract || len(response.Source) == 0 || len(response.Source) > maximumNativeSource {
		return Generated{}, errors.New("native emitter response has an unsupported contract or source size")
	}
	functions := append([]coreir.Function(nil), program.Functions...)
	sort.Slice(functions, func(first, second int) bool {
		return key(functions[first].Identity) < key(functions[second].Identity)
	})
	if len(response.Functions) != len(functions) {
		return Generated{}, errors.New("native emitter response has an incorrect binding count")
	}
	generated := Generated{Source: []byte(response.Source)}
	for index, function := range functions {
		binding := response.Functions[index]
		if !reflect.DeepEqual(binding.Identity, function.Identity) || binding.Name != name("fn_", key(function.Identity)) {
			return Generated{}, errors.New("native emitter response changed a semantic binding")
		}
		generated.Functions = append(generated.Functions, Binding{Identity: binding.Identity, Name: binding.Name})
	}
	return generated, nil
}
