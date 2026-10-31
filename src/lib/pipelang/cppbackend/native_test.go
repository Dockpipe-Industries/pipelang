package cppbackend

import (
	"strings"
	"testing"
)

func TestNativeEmitterRequiresExplicitIdentity(t *testing.T) {
	cases := []NativeEmitter{
		{Path: "relative-emitter", SHA256: strings.Repeat("a", 64)},
		{Path: "/missing/emitter", SHA256: ""},
		{Path: "/missing/emitter", SHA256: strings.Repeat("A", 64)},
		{Path: "/missing/emitter", SHA256: strings.Repeat("a", 64)},
	}
	for _, emitter := range cases {
		if err := emitter.verifyIdentity(); err == nil {
			t.Fatalf("unbound native emitter was accepted: %+v", emitter)
		}
	}
}
