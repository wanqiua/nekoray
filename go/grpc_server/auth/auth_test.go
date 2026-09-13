package auth

import (
	"context"
	"testing"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"
)

func TestAuthenticatedHandlerPanicDoesNotLeakConfig(t *testing.T) {
	ctx := metadata.NewIncomingContext(context.Background(), metadata.Pairs("nekoray_auth", "local-test-token"))
	authenticator := Authenticator{Token: "local-test-token"}
	_, err := authenticator.UnaryServerInterceptor(ctx, nil, nil, func(context.Context, any) (any, error) {
		panic("synthetic secret must not appear in error")
	})
	if status.Code(err) != codes.Internal || status.Convert(err).Message() != "core request failed" {
		t.Fatalf("unexpected panic diagnostic: %v", err)
	}
}
