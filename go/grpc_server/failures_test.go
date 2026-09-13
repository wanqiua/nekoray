package grpc_server

import (
	"context"
	"fmt"
	"grpc_server/gen"
	"grpc_server/probe"
	"os"
	"testing"
)

func TestFailureClassificationPreservesDeletionBoundary(t *testing.T) {
	ctx := context.Background()
	for _, test := range []struct {
		err   error
		local gen.FailureCategory
	}{
		{fmt.Errorf("unknown cipher method: test"), gen.FailureCategory_FailureUnsupported},
		{fmt.Errorf("QUIC is not included in this build"), gen.FailureCategory_FailureUnsupported},
		{fmt.Errorf("outbound type not found: test"), gen.FailureCategory_FailureUnsupported},
		{fmt.Errorf("missing dependency: %w", os.ErrNotExist), gen.FailureCategory_FailureCore},
		{fmt.Errorf("bad UUID"), gen.FailureCategory_FailureConfig},
	} {
		if category := LocalFailure(ctx, test.err); category != test.local {
			t.Fatalf("local %q: got %v, want %v", test.err, category, test.local)
		}
	}
	if RemoteFailure(ctx, context.DeadlineExceeded) != gen.FailureCategory_FailureRemote {
		t.Fatal("a probe's own network timeout should remain a remote failure")
	}
	cancelled, cancel := context.WithCancel(ctx)
	cancel()
	if RemoteFailure(cancelled, context.DeadlineExceeded) != gen.FailureCategory_FailureCancelled || LocalFailure(cancelled, fmt.Errorf("parse failure")) != gen.FailureCategory_FailureCancelled {
		t.Fatal("caller cancellation must win over a late test failure")
	}
	if RemoteFailure(ctx, &probe.ConfigError{Message: "bad test URL"}) != gen.FailureCategory_FailureConfig || RemoteFailure(ctx, probe.ErrCoreUnavailable) != gen.FailureCategory_FailureCore {
		t.Fatal("local test setup failures must never become dead nodes")
	}
}
