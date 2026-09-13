package grpc_server

import (
	"context"
	"errors"
	"grpc_server/gen"
	"grpc_server/probe"
	"net"
	"os"
	"strings"
)

// LocalFailure is used only before a test makes a network request. Never infer
// that a node is dead from a parser error, missing feature, or failed listener.
func LocalFailure(ctx context.Context, err error) gen.FailureCategory {
	if err == nil {
		return gen.FailureCategory_FailureNone
	}
	if ctx.Err() != nil || errors.Is(err, context.Canceled) {
		return gen.FailureCategory_FailureCancelled
	}
	if errors.Is(err, os.ErrNotExist) || errors.Is(err, os.ErrPermission) {
		return gen.FailureCategory_FailureCore
	}
	var operation *net.OpError
	if errors.As(err, &operation) {
		return gen.FailureCategory_FailureCore
	}
	message := strings.ToLower(err.Error())
	// These are diagnostics from the pinned core parsers/registries, not remote
	// server messages. A future unfamiliar diagnostic stays a configuration error.
	for _, marker := range []string{
		"not included", "not supported", "does not support", "unsupported",
		"unknown cipher method", "unknown method", "unknown transport",
		"unknown outbound", "unknown inbound", "unknown config id",
		"outbound type not found", "inbound type not found", "endpoint type not found",
		"plugin not found", "deprecated and removed", "removed in sing-box",
	} {
		if strings.Contains(message, marker) {
			return gen.FailureCategory_FailureUnsupported
		}
	}
	return gen.FailureCategory_FailureConfig
}

func RemoteFailure(ctx context.Context, err error) gen.FailureCategory {
	if err == nil {
		return gen.FailureCategory_FailureNone
	}
	if ctx.Err() != nil || errors.Is(err, context.Canceled) {
		return gen.FailureCategory_FailureCancelled
	}
	var invalid *probe.ConfigError
	if errors.As(err, &invalid) {
		return gen.FailureCategory_FailureConfig
	}
	if errors.Is(err, probe.ErrCoreUnavailable) || errors.Is(err, probe.ErrCoreNetworkChanged) {
		return gen.FailureCategory_FailureCore
	}
	return gen.FailureCategory_FailureRemote
}

func SetTestError(response *gen.TestResp, err error, category gen.FailureCategory) {
	if err == nil {
		return
	}
	response.Error = err.Error()
	response.FailureCategory = category
}
