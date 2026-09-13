// Package probe implements the GUI's HTTP latency measurement.
package probe

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"strings"
	"time"
)

const DefaultTimeout = 10 * time.Second

var ErrCoreUnavailable = errors.New("core is not running")
var ErrCoreNetworkChanged = errors.New("test interrupted by a local network change; retry the test")

// ConfigError denotes a local test setting problem, never a failed node.
type ConfigError struct{ Message string }

func (e *ConfigError) Error() string { return e.Message }

// NewHTTPClient always uses the supplied core dialer. Request contexts own the
// timeout, so a hidden transport timeout cannot cut a longer GUI test short.
func NewHTTPClient(dial func(context.Context, string, string) (net.Conn, error)) *http.Client {
	if dial == nil {
		dial = func(context.Context, string, string) (net.Conn, error) {
			return nil, ErrCoreUnavailable
		}
	}
	return &http.Client{Transport: &http.Transport{
		DialContext: dial, ForceAttemptHTTP2: true, DisableCompression: true,
		IdleConnTimeout: 30 * time.Second,
	}}
}

// URL measures time to the HTTP response headers. The legacy truehttp(s) prefix
// selects one warm-up followed by two measurements on a reusable connection.
// Each request gets its own timeout; caller cancellation covers the entire test.
func URL(ctx context.Context, client *http.Client, link string, timeoutMS int32) (int32, error) {
	if client == nil {
		return 0, ErrCoreUnavailable
	}
	defer client.CloseIdleConnections()
	link = strings.TrimSpace(link)
	warm := strings.HasPrefix(link, "truehttp://") || strings.HasPrefix(link, "truehttps://")
	if warm {
		link = strings.TrimPrefix(link, "true")
	}
	u, err := url.ParseRequestURI(link)
	if err != nil || (u.Scheme != "http" && u.Scheme != "https") || u.Host == "" {
		return 0, &ConfigError{Message: "latency test URL must start with http:// or https://"}
	}
	timeout := time.Duration(timeoutMS) * time.Millisecond
	if timeout <= 0 {
		timeout = DefaultTimeout
	}
	count := 1
	if warm {
		count = 3
	}
	var total time.Duration
	for n := 0; n < count; n++ {
		elapsed, err := request(ctx, client, link, timeout, warm && n < count-1)
		if err != nil {
			return 0, fmt.Errorf("URL test %s (request %d/%d): %w", u.Host, n+1, count, err)
		}
		if !warm || n > 0 {
			total += elapsed
		}
	}
	if warm {
		total /= 2
	}
	return int32(max(1, total.Milliseconds())), nil
}

func request(parent context.Context, client *http.Client, link string, timeout time.Duration, reuse bool) (time.Duration, error) {
	ctx, cancel := context.WithTimeout(parent, timeout)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, link, nil)
	if err != nil {
		return 0, err
	}
	req.Header.Set("Cache-Control", "no-cache")
	start := time.Now()
	response, err := client.Do(req)
	if err != nil {
		return 0, err
	}
	elapsed := time.Since(start)
	defer response.Body.Close()
	if response.StatusCode < 200 || response.StatusCode >= 400 {
		return 0, fmt.Errorf("test endpoint returned HTTP %d", response.StatusCode)
	}
	if reuse {
		// An unread HTTP/1.1 response forces a new TCP/TLS handshake. Bound the
		// drain so a download URL cannot become an unbounded latency probe.
		const bodyLimit = 64 * 1024
		n, err := io.Copy(io.Discard, io.LimitReader(response.Body, bodyLimit+1))
		if err != nil {
			return 0, err
		}
		if n > bodyLimit {
			return 0, &ConfigError{Message: "true delay requires a small response; use a 204 endpoint"}
		}
	}
	return elapsed, nil
}
