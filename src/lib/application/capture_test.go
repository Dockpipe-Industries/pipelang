package application

import (
	"io"
	"os"
	"testing"
)

func captureStdout(t *testing.T, run func() error) (string, error) {
	t.Helper()
	return captureOutput(t, &os.Stdout, run)
}

func captureResultStderr(t *testing.T, run func() error) (string, error) {
	t.Helper()
	return captureOutput(t, &os.Stderr, run)
}

func captureOutput(t *testing.T, stream **os.File, run func() error) (string, error) {
	t.Helper()
	file, err := os.CreateTemp(t.TempDir(), "output")
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()
	previous := *stream
	*stream = file
	defer func() { *stream = previous }()
	runErr := run()
	if _, err := file.Seek(0, io.SeekStart); err != nil {
		t.Fatal(err)
	}
	data, err := io.ReadAll(file)
	if err != nil {
		t.Fatal(err)
	}
	return string(data), runErr
}
