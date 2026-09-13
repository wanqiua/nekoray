package auth

import (
	"context"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"
)

// Authenticator exposes a function for authenticating requests.
type Authenticator struct {
	Token string
}

func (a Authenticator) UnaryServerInterceptor(ctx context.Context, req any, _ *grpc.UnaryServerInfo, handler grpc.UnaryHandler) (response any, failure error) {
	ctx, err := a.Authenticate(ctx)
	if err != nil {
		return nil, err
	}
	// A bad custom configuration must not terminate every running proxy/test.
	// Do not print the panic value: upstream parsers may embed credentials.
	defer func() {
		if recover() != nil {
			response = nil
			failure = status.Error(codes.Internal, "core request failed")
		}
	}()
	return handler(ctx, req)
}

type authenticatedStream struct {
	grpc.ServerStream
	ctx context.Context
}

func (s *authenticatedStream) Context() context.Context { return s.ctx }

func (a Authenticator) StreamServerInterceptor(srv any, stream grpc.ServerStream, _ *grpc.StreamServerInfo, handler grpc.StreamHandler) error {
	ctx, err := a.Authenticate(stream.Context())
	if err != nil {
		return err
	}
	return handler(srv, &authenticatedStream{ServerStream: stream, ctx: ctx})
}

// Authenticate checks that a token exists and is valid. It stores the user
// metadata in the returned context and removes the token from the context.
func (a Authenticator) Authenticate(ctx context.Context) (newCtx context.Context, err error) {
	auth, err := extractHeader(ctx, "nekoray_auth")
	if err != nil {
		return ctx, err
	}

	if auth != a.Token {
		return ctx, status.Error(codes.Unauthenticated, "invalid token")
	}

	return purgeHeader(ctx, "nekoray_auth"), nil
}

func extractHeader(ctx context.Context, header string) (string, error) {
	md, ok := metadata.FromIncomingContext(ctx)
	if !ok {
		return "", status.Error(codes.Unauthenticated, "no headers in request")
	}

	authHeaders, ok := md[header]
	if !ok {
		return "", status.Error(codes.Unauthenticated, "no header in request")
	}

	if len(authHeaders) != 1 {
		return "", status.Error(codes.Unauthenticated, "more than 1 header in request")
	}

	return authHeaders[0], nil
}

func purgeHeader(ctx context.Context, header string) context.Context {
	md, _ := metadata.FromIncomingContext(ctx)
	mdCopy := md.Copy()
	mdCopy[header] = nil
	return metadata.NewIncomingContext(ctx, mdCopy)
}
