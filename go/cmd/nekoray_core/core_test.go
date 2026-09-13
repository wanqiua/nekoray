package main

import (
	"encoding/json"
	"fmt"
	"grpc_server/testutil"
	"io"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

func TestGUIProtocolLifecycle(t *testing.T) {
	testutil.RunLifecycle(t, &server{}, func(port int) string {
		return fmt.Sprintf(`{"log":{"loglevel":"none"},"inbounds":[{"tag":"mixed-in","listen":"127.0.0.1","port":%d,"protocol":"socks","settings":{"udp":true}}],"outbounds":[{"tag":"proxy","protocol":"freedom","settings":{}}],"stats":{},"policy":{"system":{"statsOutboundUplink":true,"statsOutboundDownlink":true}}}`, port)
	}, true)
}

func TestVLESSCompatibleTransports(t *testing.T) {
	target := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { io.WriteString(w, "xray-compatible") }))
	defer target.Close()
	for _, transport := range []string{"tcp", "ws", "grpc"} {
		t.Run(transport, func(t *testing.T) {
			port := testutil.FreePort(t)
			stream := map[string]any{"network": transport}
			if transport == "ws" {
				stream["wsSettings"] = map[string]any{"path": "/test"}
			}
			if transport == "grpc" {
				stream["grpcSettings"] = map[string]any{"serviceName": "test"}
			}
			id := "75f2a015-3611-4c7f-bf7c-a948276b179f"
			serverConfig := map[string]any{
				"log":       map[string]any{"loglevel": "none"},
				"inbounds":  []any{map[string]any{"tag": "server", "listen": "127.0.0.1", "port": port, "protocol": "vless", "settings": map[string]any{"clients": []any{map[string]any{"id": id}}, "decryption": "none"}, "streamSettings": stream}},
				"outbounds": []any{map[string]any{"tag": "direct", "protocol": "freedom", "settings": map[string]any{}}},
			}
			bytes, _ := json.Marshal(serverConfig)
			inbound, err := NewNekoV2rayInstance(string(bytes))
			if err != nil {
				t.Fatal(err)
			}
			defer inbound.Close()
			if err := inbound.Start(); err != nil {
				t.Fatal(err)
			}
			clientConfig := map[string]any{
				"log":       map[string]any{"loglevel": "none"},
				"outbounds": []any{map[string]any{"tag": "proxy", "protocol": "vless", "settings": map[string]any{"vnext": []any{map[string]any{"address": "127.0.0.1", "port": port, "users": []any{map[string]any{"id": id, "encryption": "none"}}}}}, "streamSettings": stream}},
			}
			bytes, _ = json.Marshal(clientConfig)
			outbound, err := NewNekoV2rayInstance(string(bytes))
			if err != nil {
				t.Fatal(err)
			}
			defer outbound.Close()
			if err := outbound.Start(); err != nil {
				t.Fatal(err)
			}
			client := createProxyHttpClient(outbound)
			client.Timeout = 5 * time.Second
			defer client.CloseIdleConnections()
			response, err := client.Get(target.URL)
			if err != nil {
				t.Fatal(err)
			}
			defer response.Body.Close()
			payload, err := io.ReadAll(response.Body)
			if err != nil || string(payload) != "xray-compatible" {
				t.Fatalf("%q: %v", payload, err)
			}
		})
	}
}
