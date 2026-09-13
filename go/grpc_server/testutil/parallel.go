package testutil

import (
	"bufio"
	"context"
	"encoding/json"
	"fmt"
	"grpc_server/gen"
	"net"
	"net/http"
	"net/http/httptest"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"google.golang.org/grpc/metadata"
)

func runParallelURLTests(t *testing.T, client gen.LibcoreServiceClient, makeConfig func(int) string) {
	t.Helper()
	var config map[string]any
	if err := json.Unmarshal([]byte(makeConfig(0)), &config); err != nil {
		t.Fatal(err)
	}
	// Batch probes do not need a listening inbound, just like BuildConfig(forTest).
	delete(config, "inbounds")
	encoded, err := json.Marshal(config)
	if err != nil {
		t.Fatal(err)
	}
	request := func(url string) *gen.TestReq {
		return &gen.TestReq{Mode: gen.TestMode_UrlTest, Timeout: 6000, Url: url,
			Config: &gen.LoadConfigReq{CoreConfig: string(encoded)}}
	}
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	ctx = metadata.NewOutgoingContext(ctx, metadata.Pairs("nekoray_auth", "local-test-token"))

	t.Run("stop_active_proxy_preserves_independent_probe", func(t *testing.T) {
		active := &gen.LoadConfigReq{CoreConfig: makeConfig(FreePort(t))}
		if response, err := client.Start(ctx, active); err != nil || response.Error != "" {
			t.Fatalf("start active proxy: %v %v", response, err)
		}
		defer client.Stop(ctx, &gen.EmptyReq{})
		arrived := make(chan struct{}, 1)
		release := make(chan struct{})
		var releaseOnce sync.Once
		unblock := func() { releaseOnce.Do(func() { close(release) }) }
		endpoint := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			arrived <- struct{}{}
			select {
			case <-release:
				w.WriteHeader(http.StatusNoContent)
			case <-r.Context().Done():
			}
		}))
		defer endpoint.Close()
		defer unblock()
		result := make(chan error, 1)
		go func() {
			response, err := client.Test(ctx, request(endpoint.URL))
			if err == nil && (response.Error != "" || response.Ms <= 0 || response.FailureCategory != gen.FailureCategory_FailureNone) {
				err = fmt.Errorf("independent probe: %v", response)
			}
			result <- err
		}()
		select {
		case <-arrived:
		case <-time.After(5 * time.Second):
			t.Fatal("independent probe did not arrive")
		}
		if response, err := client.Check(ctx, active); err != nil || response.Error != "" {
			t.Fatalf("preflight while probing: %v %v", response, err)
		}
		if response, err := client.Stop(ctx, &gen.EmptyReq{}); err != nil || response.Error != "" {
			t.Fatalf("stop active proxy: %v %v", response, err)
		}
		unblock()
		select {
		case err := <-result:
			if err != nil {
				t.Fatal(err)
			}
		case <-time.After(3 * time.Second):
			t.Fatal("stopping the active proxy interrupted an independent probe")
		}
	})

	t.Run("eight_simultaneous_node_probes", func(t *testing.T) {
		const count = 8
		arrived := make(chan struct{}, count)
		release := make(chan struct{})
		var releaseOnce sync.Once
		unblock := func() { releaseOnce.Do(func() { close(release) }) }
		server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			arrived <- struct{}{}
			select {
			case <-release:
				w.WriteHeader(http.StatusNoContent)
			case <-r.Context().Done():
			}
		}))
		defer server.Close()
		defer unblock()
		results := make(chan error, count)
		for n := 0; n < count; n++ {
			go func() {
				response, err := client.Test(ctx, request(server.URL))
				if err == nil && (response.Error != "" || response.Ms <= 0) {
					err = fmt.Errorf("URL response: %v", response)
				}
				results <- err
			}()
		}
		deadline := time.NewTimer(5 * time.Second)
		defer deadline.Stop()
		for n := 0; n < count; n++ {
			select {
			case <-arrived:
			case <-deadline.C:
				t.Fatalf("only %d/%d probes ran simultaneously", n, count)
			}
		}
		unblock()
		for n := 0; n < count; n++ {
			if err := <-results; err != nil {
				t.Error(err)
			}
		}
	})

	t.Run("cancel_closes_proxy_request", func(t *testing.T) {
		arrived, closed := make(chan struct{}), make(chan struct{})
		server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			close(arrived)
			<-r.Context().Done()
			close(closed)
		}))
		defer server.Close()
		requestContext, requestCancel := context.WithCancel(ctx)
		defer requestCancel()
		go client.Test(requestContext, request(server.URL))
		select {
		case <-arrived:
		case <-time.After(5 * time.Second):
			t.Fatal("probe did not reach the endpoint")
		}
		requestCancel()
		select {
		case <-closed:
		case <-time.After(time.Second):
			t.Fatal("cancelled RPC left its proxy request running")
		}
	})

	t.Run("configured_timeout_exceeds_legacy_three_seconds", func(t *testing.T) {
		server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			timer := time.NewTimer(3100 * time.Millisecond)
			defer timer.Stop()
			select {
			case <-timer.C:
				w.WriteHeader(http.StatusNoContent)
			case <-r.Context().Done():
			}
		}))
		defer server.Close()
		response, err := client.Test(ctx, request(server.URL))
		if err != nil || response.Error != "" || response.Ms < 3000 {
			t.Fatalf("longer probe deadline was not honored: %v %v", response, err)
		}
	})

	t.Run("mixed_nodes_match_serial_results", func(t *testing.T) {
		// Each node is a different HTTP CONNECT proxy. The unreachable target
		// is the same for every probe, so a direct fallback cannot pass.
		type node struct {
			config string
			works  bool
			hits   *atomic.Int32
		}
		var nodes []node
		for n := 0; n < 8; n++ {
			kind := n % 4 // fast, slow, rejected CONNECT, stalled response
			hits := new(atomic.Int32)
			var handlers sync.WaitGroup
			proxy := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				hits.Add(1)
				if r.Method != http.MethodConnect || r.Host != "127.0.0.1:1" {
					w.WriteHeader(http.StatusBadRequest)
					return
				}
				if kind == 2 {
					w.WriteHeader(http.StatusForbidden)
					return
				}
				handlers.Add(1)
				defer handlers.Done()
				connection, reader, err := w.(http.Hijacker).Hijack()
				if err != nil {
					return
				}
				defer connection.Close()
				connection.SetDeadline(time.Now().Add(3 * time.Second))
				fmt.Fprint(connection, "HTTP/1.1 200 Connection Established\r\n\r\n")
				probe, err := http.ReadRequest(bufio.NewReader(reader))
				if err != nil {
					return
				}
				probe.Body.Close()
				if kind == 1 {
					time.Sleep(150 * time.Millisecond)
				} else if kind == 3 {
					// Wait for the probe to cancel and close the tunnel.
					var b [1]byte
					connection.Read(b[:])
					return
				}
				fmt.Fprint(connection, "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n")
			}))
			t.Cleanup(func() { proxy.Close(); handlers.Wait() })
			port := proxy.Listener.Addr().(*net.TCPAddr).Port
			var outbound map[string]any
			if config["outbounds"].([]any)[0].(map[string]any)["protocol"] != nil {
				outbound = map[string]any{"tag": "proxy", "protocol": "http", "settings": map[string]any{
					"servers": []any{map[string]any{"address": "127.0.0.1", "port": port}},
				}}
			} else {
				outbound = map[string]any{"tag": "proxy", "type": "http", "server": "127.0.0.1", "server_port": port}
			}
			encoded, err := json.Marshal(map[string]any{"outbounds": []any{outbound}})
			if err != nil {
				t.Fatal(err)
			}
			nodes = append(nodes, node{string(encoded), kind < 2, hits})
		}
		// A bad configuration must not prevent later nodes from completing.
		nodes = append(nodes, node{`{"outbounds":[{"type":"invalid","protocol":"invalid"}]}`, false, nil})
		testNode := func(n node) error {
			response, err := client.Test(ctx, &gen.TestReq{Mode: gen.TestMode_UrlTest, Timeout: 900,
				Url: "http://127.0.0.1:1/generate_204", Config: &gen.LoadConfigReq{CoreConfig: n.config}})
			if err != nil {
				return err
			}
			works := response.Error == "" && response.Ms > 0
			if works != n.works {
				return fmt.Errorf("expected healthy=%t, got %v", n.works, response)
			}
			return nil
		}
		for index, n := range nodes {
			if err := testNode(n); err != nil {
				t.Fatalf("serial node %d: %v", index, err)
			}
		}
		results := make(chan error, len(nodes))
		for index, n := range nodes {
			go func(index int, n node) {
				err := testNode(n)
				if err != nil {
					err = fmt.Errorf("batch node %d: %w", index, err)
				}
				results <- err
			}(index, n)
		}
		for range nodes {
			if err := <-results; err != nil {
				t.Error(err)
			}
		}
		for index, n := range nodes {
			if n.hits != nil && n.hits.Load() < 2 {
				t.Errorf("node %d did not use its own proxy in both runs", index)
			}
		}
	})
}
