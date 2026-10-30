package forgepipe_test

import (
	"bytes"
	"encoding/json"
	"fmt"
	"math"
	"reflect"
	"strings"
	"testing"

	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview"
	"github.com/Dockpipe-Industries/pipelang/sdk/pipeview/forgepipe"
)

func TestEncodePreservesPortableValuesAndEventReferences(t *testing.T) {
	text := "\ufeffRider\x00e\u0301🏍"
	document, err := pipeview.Compile(pipeview.Column("root",
		pipeview.Label("minimum-label", "Min: ", pipeview.State("minimum", int64(math.MinInt64))),
		pipeview.Label("maximum-label", "Max: ", pipeview.State("maximum", int64(math.MaxInt64))),
		pipeview.TextField("name-field", pipeview.State("name", text), pipeview.Change[string]("name-edited")),
		pipeview.Toggle("enabled-toggle", "Enabled", pipeview.State("enabled", true), pipeview.Change[bool]("enabled-edited")),
		pipeview.Row("actions", pipeview.Button("reset-button", "Reset", pipeview.Click("reset")))))
	if err != nil {
		t.Fatal(err)
	}
	encoded, err := forgepipe.Encode(document, forgepipe.Options{Scale: 1.125, Focus: "#FFad45FF"})
	if err != nil {
		t.Fatal(err)
	}
	decoder := json.NewDecoder(bytes.NewReader(encoded))
	decoder.UseNumber()
	var actual any
	if err := decoder.Decode(&actual); err != nil {
		t.Fatal(err)
	}
	// Independent transport expectation: no float64 conversion for integer state,
	// no semantic event catalogue injected into the backend's declaration schema.
	expectedJSON := `{
        "schema":"forgepipe.ui.v1",
        "root":{"id":"root","kind":"column","children":[
            {"id":"minimum-label","kind":"label","text":"Min: ","binding":"minimum"},
            {"id":"maximum-label","kind":"label","text":"Max: ","binding":"maximum"},
            {"id":"name-field","kind":"text-field","binding":"name","action":"name-edited"},
            {"id":"enabled-toggle","kind":"toggle","text":"Enabled","binding":"enabled","action":"enabled-edited"},
            {"id":"actions","kind":"row","children":[
                {"id":"reset-button","kind":"button","text":"Reset","action":"reset"}
            ]}
        ]},
        "style":{"scale":1.125,"focus":"#FFad45FF"},
        "state":{"minimum":-9223372036854775808,"maximum":9223372036854775807,
            "enabled":true,"name":"\ufeffRider\u0000e\u0301🏍"}
    }`
	expectedDecoder := json.NewDecoder(bytes.NewBufferString(expectedJSON))
	expectedDecoder.UseNumber()
	var expected any
	if err := expectedDecoder.Decode(&expected); err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(actual, expected) {
		t.Fatalf("transport mismatch:\n got %s\nwant %s", encoded, expectedJSON)
	}
	repeated, err := forgepipe.Encode(document, forgepipe.Options{Scale: 1.125, Focus: "#FFad45FF"})
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(encoded, repeated) {
		t.Fatal("identical declarations produced different transport bytes")
	}
}

func TestEncodeRefusesInvalidOptionsAndZeroDocument(t *testing.T) {
	document, err := pipeview.Compile(pipeview.StaticText("label", "text"))
	if err != nil {
		t.Fatal(err)
	}
	cases := map[string]forgepipe.Options{
		"zero":                 {},
		"below scale minimum":  {Scale: 0.499, Focus: "#11223344"},
		"above scale maximum":  {Scale: 3.001, Focus: "#11223344"},
		"nan":                  {Scale: math.NaN(), Focus: "#11223344"},
		"positive infinity":    {Scale: math.Inf(1), Focus: "#11223344"},
		"negative infinity":    {Scale: math.Inf(-1), Focus: "#11223344"},
		"empty color":          {Scale: 1},
		"short color":          {Scale: 1, Focus: "#112233"},
		"missing color prefix": {Scale: 1, Focus: "11223344"},
		"nonhex color":         {Scale: 1, Focus: "#112233GG"},
		"trailing color bytes": {Scale: 1, Focus: "#11223344\n"},
	}
	for name, options := range cases {
		t.Run(name, func(t *testing.T) {
			encoded, err := forgepipe.Encode(document, options)
			if err == nil || len(encoded) != 0 {
				t.Fatalf("invalid options produced transport: %q, %v", encoded, err)
			}
		})
	}
	encoded, err := forgepipe.Encode(pipeview.Document{}, forgepipe.Options{Scale: 1, Focus: "#11223344"})
	if err == nil || len(encoded) != 0 {
		t.Fatalf("zero document produced transport: %q, %v", encoded, err)
	}
}

func TestEncodeAcceptsScaleEndpoints(t *testing.T) {
	document, err := pipeview.Compile(pipeview.StaticText("label", ""))
	if err != nil {
		t.Fatal(err)
	}
	for _, scale := range []float64{0.5, 3} {
		if _, err := forgepipe.Encode(document, forgepipe.Options{Scale: scale, Focus: "#aAbBcCdD"}); err != nil {
			t.Fatalf("scale endpoint %v refused: %v", scale, err)
		}
	}
}

func TestEncodeRefusesTransportBeyondInstalledSDKCapacity(t *testing.T) {
	// Every control is neutral-profile valid, but the generated backend document
	// exceeds ForgePipe's 65536-byte input capacity.
	children := make([]pipeview.Node, 0, 20)
	for index := 0; index < 20; index++ {
		children = append(children, pipeview.StaticText(fmt.Sprintf("label-%d", index), strings.Repeat("a", 4096)))
	}
	document, err := pipeview.Compile(pipeview.Column("root", children...))
	if err != nil {
		t.Fatal(err)
	}
	encoded, err := forgepipe.Encode(document, forgepipe.Options{Scale: 1, Focus: "#11223344"})
	if err == nil || len(encoded) != 0 {
		t.Fatalf("backend input capacity exceeded without refusal: bytes=%d error=%v", len(encoded), err)
	}
}

func TestEncodeAppliesBackendIdentityCapacity(t *testing.T) {
	for _, length := range []int{64, 65} {
		identity := strings.Repeat("x", length)
		cases := map[string]pipeview.Node{
			"control": pipeview.StaticText(identity, ""),
			"binding": pipeview.Label("label", "", pipeview.State(identity, int64(1))),
			"event":   pipeview.Button("button", "", pipeview.Click(identity)),
		}
		for name, root := range cases {
			t.Run(fmt.Sprintf("%s-%d", name, length), func(t *testing.T) {
				document, err := pipeview.Compile(root)
				if err != nil {
					t.Fatalf("backend-specific identity limit leaked into neutral admission: %v", err)
				}
				encoded, err := forgepipe.Encode(document, forgepipe.Options{Scale: 1, Focus: "#11223344"})
				if length == 64 {
					if err != nil || len(encoded) == 0 {
						t.Fatalf("64-byte identity refused: %v", err)
					}
				} else if err == nil || len(encoded) != 0 {
					t.Fatalf("65-byte identity produced transport: %q, %v", encoded, err)
				}
			})
		}
	}
}
