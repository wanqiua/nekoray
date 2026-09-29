package main

import (
	"context"
	"fmt"
	"grpc_server/gen"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestCheckLegacyFeaturesAndUnavailableProtocols(t *testing.T) {
	s := &server{}
	for _, test := range []struct {
		name     string
		config   string
		category gen.FailureCategory
	}{
		{"rc4-md5", `{"outbounds":[{"tag":"proxy","protocol":"shadowsocks","settings":{"servers":[{"address":"127.0.0.1","port":12345,"method":"rc4-md5","password":"local-test"}]}}]}`, gen.FailureCategory_FailureNone},
		{"bf-cfb", `{"outbounds":[{"tag":"proxy","protocol":"shadowsocks","settings":{"servers":[{"address":"127.0.0.1","port":12345,"method":"bf-cfb","password":"local-test"}]}}]}`, gen.FailureCategory_FailureNone},
		{"allowInsecure", `{"outbounds":[{"protocol":"vless","settings":{"vnext":[{"address":"127.0.0.1","port":12345,"users":[{"id":"75f2a015-3611-4c7f-bf7c-a948276b179f","encryption":"none"}]}]},"streamSettings":{"network":"ws","security":"tls","tlsSettings":{"allowInsecure":true}}}]}`, gen.FailureCategory_FailureNone},
		{"unavailable protocol", `{"outbounds":[{"protocol":"hysteria2","settings":{}}]}`, gen.FailureCategory_FailureUnsupported},
		{"invalid modern pin", `{"outbounds":[{"protocol":"freedom","streamSettings":{"security":"tls","tlsSettings":{"pinnedPeerCertSha256":"not-a-real-pin"}}}]}`, gen.FailureCategory_FailureConfig},
	} {
		t.Run(test.name, func(t *testing.T) {
			response, err := s.Check(context.Background(), &gen.LoadConfigReq{CoreConfig: test.config})
			if err != nil || response.FailureCategory != test.category || (response.Error == "") != (test.category == gen.FailureCategory_FailureNone) {
				t.Fatalf("Check: %v %v", response, err)
			}
		})
	}
}

func TestCheckNeverConstructsXrayLogRuntime(t *testing.T) {
	path := filepath.Join(t.TempDir(), "check-must-not-create-this.log")
	config := fmt.Sprintf(`{"log":{"loglevel":"warning","error":%q},"outbounds":[{"protocol":"freedom","settings":{}}]}`, filepath.ToSlash(path))
	if _, err := checkXrayConfig(config); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(path); !os.IsNotExist(err) {
		t.Fatal("Check constructed the Xray logging runtime")
	}
}

func TestCancelledRequestHasCategoryAndIdentity(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	response, err := (&server{}).Test(ctx, &gen.TestReq{Mode: gen.TestMode_TcpPing, Address: "127.0.0.1:1"})
	if err != nil || response.FailureCategory != gen.FailureCategory_FailureCancelled || response.CoreFamily != "xray" || !strings.HasPrefix(response.BuildId, "sha256:") {
		t.Fatalf("cancelled test: %v %v", response, err)
	}
}
