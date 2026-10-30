package state

import _ "embed"

const Contract = "pipelang.pipeview.state.v1"

// NativeRuntime is the bounded C++ companion of the Go reference state model.
// It is embedded into explicitly selected action-profile artifacts, with no
// runtime compiler discovery or dependency on a renderer.
//
//go:embed native/runtime.hpp
var nativeRuntime string

func NativeRuntime() string {
	return nativeRuntime
}
