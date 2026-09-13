package main

import (
	"context"
	"errors"
	"grpc_server/probe"
	"io"
	"net/http"
	"strings"
	"testing"
	"time"
)

type probeTransportFunc func(*http.Request) (*http.Response, error)

func (f probeTransportFunc) RoundTrip(r *http.Request) (*http.Response, error) { return f(r) }

func TestStartupProbeRetriesOnlyLocalReset(t *testing.T) {
	for _, test := range []struct {
		name     string
		failure  error
		age      time.Duration
		attempts int
	}{
		{"initial network reset", errors.New("network changed"), 0, 2},
		{"remote EOF", io.EOF, 0, 1},
		{"remote certificate", errors.New("x509: certificate signed by unknown authority"), 0, 1},
		{"network reset after startup", errors.New("network changed"), 10 * time.Second, 1},
	} {
		t.Run(test.name, func(t *testing.T) {
			attempts := 0
			transport := &startupRetryTransport{instance: &BoxInstance{startedAt: time.Now().Add(-test.age)}, base: probeTransportFunc(func(*http.Request) (*http.Response, error) {
				attempts++
				if attempts == 1 {
					return nil, test.failure
				}
				return &http.Response{StatusCode: http.StatusNoContent, Body: io.NopCloser(strings.NewReader("")), Header: make(http.Header)}, nil
			})}
			request, _ := http.NewRequest(http.MethodGet, "http://127.0.0.1/", nil)
			response, err := transport.RoundTrip(request)
			if response != nil {
				response.Body.Close()
			}
			if attempts != test.attempts || (err == nil) != (test.attempts == 2) {
				t.Fatalf("attempts %d; error %v", attempts, err)
			}
			if test.name == "network reset after startup" && !errors.Is(err, probe.ErrCoreNetworkChanged) {
				t.Fatal("local invalidation must not be reported as a dead remote node")
			}
		})
	}
}

func TestStartupProbeCancellationPreventsRetry(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	attempts := 0
	transport := &startupRetryTransport{instance: &BoxInstance{startedAt: time.Now()}, base: probeTransportFunc(func(*http.Request) (*http.Response, error) {
		attempts++
		cancel()
		return nil, errors.New("network changed")
	})}
	request, _ := http.NewRequestWithContext(ctx, http.MethodGet, "http://127.0.0.1/", nil)
	transport.RoundTrip(request)
	if attempts != 1 {
		t.Fatal("a cancelled probe was retried")
	}
}
