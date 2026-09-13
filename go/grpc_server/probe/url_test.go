package probe

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

func localClient() *http.Client {
	return NewHTTPClient((&net.Dialer{}).DialContext)
}

func TestTrueDelayReusesConnectionWithSeparateTimeouts(t *testing.T) {
	var connections atomic.Int32
	server := httptest.NewUnstartedServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		time.Sleep(80 * time.Millisecond)
		io.WriteString(w, "small probe response")
	}))
	server.Config.ConnState = func(_ net.Conn, state http.ConnState) {
		if state == http.StateNew {
			connections.Add(1)
		}
	}
	server.Start()
	defer server.Close()
	ms, err := URL(context.Background(), localClient(), "true"+server.URL, 200)
	if err != nil || ms < 60 || ms > 200 {
		t.Fatalf("warm latency = %d: %v", ms, err)
	}
	if n := connections.Load(); n != 1 {
		t.Fatalf("expected a reused TCP connection, opened %d", n)
	}
}

func TestURLHonorsCallerCancellation(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		<-r.Context().Done()
	}))
	defer server.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Millisecond)
	defer cancel()
	start := time.Now()
	_, err := URL(ctx, localClient(), server.URL, 10000)
	if !errors.Is(err, context.DeadlineExceeded) || time.Since(start) > time.Second {
		t.Fatalf("caller deadline: %v (%s)", err, time.Since(start))
	}
}

func TestURLMeasuresHeadersWithoutDownloadingBody(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
		w.(http.Flusher).Flush()
		<-r.Context().Done()
	}))
	defer server.Close()
	ms, err := URL(context.Background(), localClient(), server.URL, 500)
	if err != nil || ms <= 0 || ms >= 500 {
		t.Fatalf("header latency = %d: %v", ms, err)
	}
}

func TestURLReportsEndpointErrors(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusServiceUnavailable)
	}))
	defer server.Close()
	_, err := URL(context.Background(), localClient(), server.URL, 500)
	if err == nil || !strings.Contains(err.Error(), "HTTP 503") {
		t.Fatalf("endpoint error: %v", err)
	}
	for _, value := range []string{"", "truegarbage", "ftp://example.com/"} {
		if _, err := URL(context.Background(), localClient(), value, 500); err == nil {
			t.Fatalf("accepted invalid URL %q", value)
		}
	}
}

func TestMissingCoreDoesNotFallBackToDirect(t *testing.T) {
	var requests atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		requests.Add(1)
		fmt.Fprintln(w, "must not be reached")
	}))
	defer server.Close()
	_, err := URL(context.Background(), NewHTTPClient(nil), server.URL, 500)
	if err == nil || requests.Load() != 0 {
		t.Fatalf("missing core dialer used direct networking: %v", err)
	}
}
