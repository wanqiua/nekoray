package main

import (
	"context"
	"errors"
	"grpc_server/gen"
	"testing"
)

func TestStopPreservesUnconfirmedCleanup(t *testing.T) {
	// Model an upstream Close failure already cached by closeOnce, without
	// starting a real listener or touching host networking.
	failure := errors.New("injected resource cleanup failure")
	failed := &NekoV2RayInstance{closeError: failure}
	failed.closeOnce.Do(func() { failed.closed.Store(true) })
	instanceMu.Lock()
	previous := instance
	instance = failed
	instanceMu.Unlock()
	t.Cleanup(func() {
		instanceMu.Lock()
		instance = previous
		instanceMu.Unlock()
	})
	s := &server{}
	for attempt := 1; attempt <= 2; attempt++ {
		response, err := s.Stop(context.Background(), &gen.EmptyReq{})
		if err != nil || response.GetError() != failure.Error() {
			t.Fatalf("Stop %d hid the cleanup failure: %v, %q", attempt, err, response.GetError())
		}
		if currentInstance() != failed {
			t.Fatalf("Stop %d discarded ownership of the unconfirmed instance", attempt)
		}
		response, err = s.Start(context.Background(), &gen.LoadConfigReq{CoreConfig: "{"})
		if err != nil || response.GetError() != "instance already started" || currentInstance() != failed {
			t.Fatalf("Start after Stop %d replaced or ignored failed cleanup: %v, %q", attempt, err, response.GetError())
		}
	}
}
