package main

import (
	"strings"
	"testing"
)

func TestRealCompilerExportsTypedDeclarationAndCheckedAction(t *testing.T) {
	source := []byte(`public Record Control { public string Id; public string Text; }
public Class Application {
    public List<Control> View(Control control)=>list(control);
    public Result<int,ArithmeticError> Increment(int current)=>current+1;
}`)
	generated, receipt, err := compile(source, "View.pipe", "example.ui", "app.root")
	if err != nil {
		t.Fatal(err)
	}
	if receipt.Profile != "pipelang.cpp.pilot.v1" || receipt.SourceSHA256 != digest(source) ||
		receipt.GeneratedSHA256 != digest(generated) {
		t.Fatal("compiler/profile/source/generated identity mismatch")
	}
	if len(receipt.Functions) != 2 || !strings.Contains(string(generated), "std::vector<") {
		t.Fatal("real typed list declaration was not generated")
	}
	for _, function := range receipt.Functions {
		if function.Identity.PackageID != "example.ui" || !strings.HasPrefix(function.CppSymbol, "pipelang::fn_") {
			t.Fatal("missing semantic identity or actual generated binding")
		}
	}
	again, repeated, err := compile(source, "View.pipe", "example.ui", "app.root")
	if err != nil || string(again) != string(generated) || repeated.GeneratedSHA256 != receipt.GeneratedSHA256 {
		t.Fatal("non-deterministic compilation")
	}
}

func TestRefusesMalformedUnsupportedAndOversizedInput(t *testing.T) {
	cases := [][]byte{
		nil,
		[]byte(strings.Repeat("x", maximumSourceBytes+1)),
		[]byte(`public Class Broken { not valid }`),
		[]byte(`public Class Unsupported { public float Value(float x)=>x; }`),
	}
	for _, source := range cases {
		if _, _, err := compile(source, "Invalid.pipe", "example.ui", "app.root"); err == nil {
			t.Fatal("unsupported native C++ input was admitted")
		}
	}
}
