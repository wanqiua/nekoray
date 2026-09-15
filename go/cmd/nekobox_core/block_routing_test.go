package main

import (
	"context"
	"encoding/json"
	"grpc_server/testutil"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestBlockRulesRejectWithoutPermissionErrors(t *testing.T) {
	port := testutil.FreePort(t)
	logPath := filepath.Join(t.TempDir(), "routing.log")
	content, err := json.Marshal(map[string]any{
		"log":       map[string]any{"level": "debug", "output": logPath},
		"inbounds":  []any{map[string]any{"type": "socks", "tag": "local", "listen": "127.0.0.1", "listen_port": port}},
		"outbounds": []any{map[string]any{"type": "direct", "tag": "direct"}, map[string]any{"type": "block", "tag": "block"}},
		"route":     map[string]any{"final": "direct", "rules": []any{map[string]any{"domain": []string{"blocked.example.test"}, "outbound": "block"}}},
	})
	if err != nil {
		t.Fatal(err)
	}
	router, err := NewBoxInstance(content, nil)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { router.Close() })
	client := startProtocolPeerCore(t, map[string]any{"type": "socks", "tag": "proxy", "server": "127.0.0.1", "server_port": port}, false)
	verifyProtocolPeerHTTP(t, client, 1)
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	connection, err := client.DialContext(ctx, "tcp", "blocked.example.test:443")
	if connection != nil {
		connection.Close()
	}
	if err == nil {
		t.Fatal("block rule allowed the connection")
	}
	if err = router.Close(); err != nil {
		t.Fatal(err)
	}
	output, err := os.ReadFile(logPath)
	if err != nil {
		t.Fatal(err)
	}
	text := string(output)
	if strings.Contains(text, "operation not permitted") || strings.Contains(text, "ERROR") {
		t.Fatalf("normal blocking produced an error log: %s", text)
	}
	if !strings.Contains(text, "reject") {
		t.Fatal("routing did not record the rejection")
	}
}
