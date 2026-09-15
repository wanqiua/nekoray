package main

import (
	"context"
	"encoding/json"
	"fmt"
	"net"
	"testing"
	"time"

	M "github.com/sagernet/sing/common/metadata"
)

// A second sing-box client uses SOCKS to enter the actual routing path of
// the protocol client. Direct Outbound.DialContext/ListenPacket calls do not
// enforce Network(), so those alone cannot test tcp-only/udp-only settings.
func routedProtocolPeerClient(t *testing.T, out map[string]any) *BoxInstance {
	t.Helper()
	listener, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	port := listener.Addr().(*net.TCPAddr).Port
	listener.Close()
	config := map[string]any{
		"log": map[string]any{"level": "error"},
		"inbounds": []any{map[string]any{
			"type": "socks", "tag": "routed-test", "listen": "127.0.0.1", "listen_port": port,
		}},
		"outbounds": []any{out},
		"route":     map[string]any{"final": "proxy"},
	}
	content, err := json.Marshal(config)
	if err != nil {
		t.Fatal(err)
	}
	core, err := NewBoxInstance(content, nil)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := core.Close(); err != nil {
			t.Errorf("close routed protocol peer: %v", err)
		}
	})
	// Settle the core's initial QUIC interface notification via its normal
	// idempotent URL probe, before testing the separate router restrictions.
	verifyProtocolPeerHTTP(t, core, 1)
	return startProtocolPeerCore(t, map[string]any{
		"type": "socks", "tag": "proxy", "version": "5", "server": "127.0.0.1", "server_port": port,
	}, false)
}

func verifyProtocolPeerUDPRejected(t *testing.T, core *BoxInstance) {
	t.Helper()
	target, err := net.ListenPacket("udp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer target.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	conn, err := core.Outbound().Default().ListenPacket(ctx, M.SocksaddrFromNet(target.LocalAddr()))
	if err != nil {
		t.Fatalf("create SOCKS association before routing UDP: %v", err)
	}
	defer conn.Close()
	if _, err := conn.WriteTo([]byte("must-not-arrive"), target.LocalAddr()); err != nil {
		t.Fatal(err)
	}
	target.SetReadDeadline(time.Now().Add(250 * time.Millisecond))
	buffer := make([]byte, 1500)
	if count, source, err := target.ReadFrom(buffer); err == nil {
		t.Fatalf("tcp-only node sent %d UDP bytes to target from %v", count, source)
	} else if timeout, ok := err.(net.Error); !ok || !timeout.Timeout() {
		t.Fatalf("UDP target fixture failed: %v", err)
	}
}

func TestProtocolPeerRoutedNetworkRestrictions(t *testing.T) {
	for _, protocol := range []string{"snell4", "snell6", "hysteria2"} {
		for _, network := range []string{"both", "tcp", "udp"} {
			t.Run(fmt.Sprintf("%s/%s", protocol, network), func(t *testing.T) {
				name := protocol
				if protocol == "snell4" || protocol == "snell6" {
					name = "snell"
				}
				in, out := protocolPeerOptions(t, name)
				if protocol == "snell6" {
					in["version"], out["version"] = 6, 6
				}
				if network != "both" {
					out["network"] = network
				}
				startProtocolPeerCore(t, in, true)
				client := routedProtocolPeerClient(t, out)
				if network == "udp" {
					verifyProtocolPeerHTTPFailure(t, client, "")
				} else {
					verifyProtocolPeerHTTP(t, client, 1)
				}
				if network == "tcp" {
					verifyProtocolPeerUDPRejected(t, client)
				} else {
					verifyProtocolPeerUDP(t, client)
				}
			})
		}
	}
}
