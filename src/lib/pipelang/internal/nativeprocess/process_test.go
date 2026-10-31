package nativeprocess

import "testing"

func TestOutputRetentionDrainsBeyondLimit(t *testing.T) {
	output := &boundedOutput{limit: 4}
	for _, data := range []string{"abc", "def", "ghi"} {
		count, err := output.Write([]byte(data))
		if err != nil || count != len(data) {
			t.Fatal("child pipe was not fully drained")
		}
	}
	if !output.overflow || output.String() != "abcd" {
		t.Fatal("child output exceeded the retained bound")
	}
}
