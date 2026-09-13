package main

import (
	"fmt"
	"grpc_server/probe"
	"net/http"
	"strings"
	"time"
)

// In sing-box 1.14.0 the initial interface callback is asynchronous. It can
// reset a QUIC client just after Box.Start returns and cut off its first HTTP
// probe. Retry that explicitly local reset once, within the original request
// context. No sleep, broader timeout, different core, or remote-error retry.
type startupRetryTransport struct {
	base     http.RoundTripper
	instance *BoxInstance
}

func isLocalNetworkReset(err error) bool {
	if err == nil {
		return false
	}
	return strings.HasSuffix(err.Error(), "network changed") || isLocalQUICClose(err)
}

func (t *startupRetryTransport) RoundTrip(request *http.Request) (*http.Response, error) {
	response, err := t.base.RoundTrip(request)
	if !isLocalNetworkReset(err) {
		return response, err
	}
	if response != nil && response.Body != nil {
		response.Body.Close()
	}
	if request.Context().Err() == nil && !t.instance.closed.Load() &&
		time.Since(t.instance.startedAt) < 2*time.Second &&
		(request.Method == http.MethodGet || request.Method == http.MethodHead) &&
		(request.Body == nil || request.Body == http.NoBody) {
		response, err = t.base.RoundTrip(request.Clone(request.Context()))
	}
	if isLocalNetworkReset(err) {
		return response, fmt.Errorf("%w: %v", probe.ErrCoreNetworkChanged, err)
	}
	return response, err
}

func (t *startupRetryTransport) CloseIdleConnections() {
	if transport, ok := t.base.(interface{ CloseIdleConnections() }); ok {
		transport.CloseIdleConnections()
	}
}
