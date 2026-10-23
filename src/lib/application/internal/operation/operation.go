// Package operation emits the public Dockpipe operation JSON contract without
// depending on the Dockpipe engine. These events describe compiler operations.
package operation

import (
	"bufio"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"time"
)

const (
	DockpipeDirRel         = "bin/.dockpipe"
	EnvDockpipeEventLog    = "DOCKPIPE_EVENT_LOG"
	OperationEventSchemaV1 = "dockpipe.operation_event.v1"
	OperationEventKind     = "operation_result"
	OperationStatusDone    = "done"
	OperationStatusFail    = "fail"
)

type OperationOptions struct{ Spinner bool }

type Event struct {
	Schema    string            `json:"schema"`
	Type      string            `json:"type"`
	Timestamp string            `json:"ts"`
	Unit      string            `json:"unit"`
	Status    string            `json:"status"`
	IDs       map[string]string `json:"ids,omitempty"`
	Error     string            `json:"error,omitempty"`
}

func RunOperationWithOptions(output io.Writer, unit, message string, ids map[string]string, options OperationOptions, run func() error) error {
	err := run()
	event := Event{Schema: OperationEventSchemaV1, Type: OperationEventKind, Timestamp: time.Now().UTC().Format(time.RFC3339Nano), Unit: unit, Status: OperationStatusDone, IDs: ids}
	if err != nil {
		event.Status = OperationStatusFail
		event.Error = err.Error()
	}
	fmt.Fprintf(output, "%s: %s\n", unit, event.Status)
	if path := os.Getenv(EnvDockpipeEventLog); path != "" {
		if logErr := appendEvent(path, event); logErr != nil {
			return fmt.Errorf("record %s event: %w", unit, logErr)
		}
	}
	return err
}

func appendEvent(path string, event Event) error {
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	file, err := os.OpenFile(path, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o600)
	if err != nil {
		return err
	}
	encodeErr := json.NewEncoder(file).Encode(event)
	closeErr := file.Close()
	if encodeErr != nil {
		return encodeErr
	}
	return closeErr
}

func ReadOperationEvents(path string) ([]Event, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	events := []Event{}
	scanner := bufio.NewScanner(file)
	for scanner.Scan() {
		var event Event
		if err := json.Unmarshal(scanner.Bytes(), &event); err != nil {
			return nil, err
		}
		events = append(events, event)
	}
	return events, scanner.Err()
}
