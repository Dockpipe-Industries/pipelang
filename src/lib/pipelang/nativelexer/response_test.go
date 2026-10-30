package nativelexer

import (
	"strings"
	"testing"
)

func TestResponseAdmissionRejectsAmbiguousAndIncompleteObjects(t *testing.T) {
	valid := `{"contract":"pipelang.lexer.response.v1","tokens":[{"kind":"eof","literal":"","start":0,"end":0}],"failure":null}`
	if _, err := decodeResponse([]byte(valid)); err != nil {
		t.Fatal(err)
	}
	cases := map[string]string{
		"missing outcome":         strings.Replace(valid, `,"failure":null`, "", 1),
		"duplicate outcome":       strings.Replace(valid, `"tokens":`, `"tokens":[],"tokens":`, 1),
		"case alias":              strings.Replace(valid, `"tokens":`, `"Tokens":`, 1),
		"missing start":           strings.Replace(valid, `,"start":0`, "", 1),
		"duplicate offset":        strings.Replace(valid, `"start":0`, `"start":1,"sta\u0072t":0`, 1),
		"null offset":             strings.Replace(valid, `"start":0`, `"start":null`, 1),
		"fractional offset":       strings.Replace(valid, `"start":0`, `"start":0.0`, 1),
		"null literal":            strings.Replace(valid, `"literal":""`, `"literal":null`, 1),
		"unknown token field":     strings.Replace(valid, `"start":0`, `"extra":true,"start":0`, 1),
		"unpaired high surrogate": strings.Replace(valid, `"literal":""`, `"literal":"\ud800"`, 1),
		"unpaired low surrogate":  strings.Replace(valid, `"literal":""`, `"literal":"\udc00"`, 1),
		"both null":               `{"contract":"pipelang.lexer.response.v1","tokens":null,"failure":null}`,
		"both outcomes":           `{"contract":"pipelang.lexer.response.v1","tokens":[],"failure":{"code":"PL1001","message":"invalid","start":0,"end":1}}`,
		"missing failure message": `{"contract":"pipelang.lexer.response.v1","tokens":null,"failure":{"code":"PL1001","start":0,"end":1}}`,
		"trailing document":       valid + `{}`,
	}
	for name, input := range cases {
		t.Run(name, func(t *testing.T) {
			if _, err := decodeResponse([]byte(input)); err == nil {
				t.Fatal("ambiguous or malformed native response was admitted")
			}
		})
	}
}

func TestResponseStringsPreserveScalarPairsAndLiteralBackslashes(t *testing.T) {
	for input, expected := range map[string]string{
		`"\ud83c\udfcd"`: "🏍",
		`"\\ud800"`:      `\ud800`,
		`"\u2028"`:       "\u2028",
		`"\u0000"`:       "\x00",
	} {
		actual, err := wireString([]byte(input))
		if err != nil || actual != expected {
			t.Fatalf("response scalar preservation failed: %q => %q (%v)", input, actual, err)
		}
	}
}
