package main

import (
	"context"
	"encoding/json"
	"grpc_server/gen"
	"net/http"
	"net/http/httptest"
	"sync/atomic"
	"testing"
)

func TestProbeNetworkApplicability(t *testing.T) {
	for _, test := range []struct {
		name     string
		networks []string
		request  gen.TestReq
		allowed  bool
	}{
		{"udp-only/url", []string{"udp"}, gen.TestReq{Mode: gen.TestMode_UrlTest}, false},
		{"udp-only/full-latency", []string{"udp"}, gen.TestReq{Mode: gen.TestMode_FullTest, FullLatency: true}, false},
		{"udp-only/full-speed", []string{"udp"}, gen.TestReq{Mode: gen.TestMode_FullTest, FullSpeed: true}, false},
		{"udp-only/full-ip", []string{"udp"}, gen.TestReq{Mode: gen.TestMode_FullTest, FullInOut: true}, false},
		{"udp-only/full-udp", []string{"udp"}, gen.TestReq{Mode: gen.TestMode_FullTest, FullUdpLatency: true}, true},
		{"tcp-only/full-udp", []string{"tcp"}, gen.TestReq{Mode: gen.TestMode_FullTest, FullUdpLatency: true}, false},
		{"tcp-only/url", []string{"tcp"}, gen.TestReq{Mode: gen.TestMode_UrlTest}, true},
		{"both/full", []string{"tcp", "udp"}, gen.TestReq{Mode: gen.TestMode_FullTest, FullLatency: true, FullUdpLatency: true}, true},
		{"endpoint-ping", []string{"udp"}, gen.TestReq{Mode: gen.TestMode_TcpPing}, true},
	} {
		t.Run(test.name, func(t *testing.T) {
			if err := testNetworkError(test.networks, &test.request); (err == nil) != test.allowed {
				t.Fatalf("applicability error=%v, allowed=%v", err, test.allowed)
			}
		})
	}
}

func TestRestrictedSnellURLProbeIsUnsupported(t *testing.T) {
	var requests atomic.Int32
	endpoint := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		requests.Add(1)
		w.WriteHeader(http.StatusNoContent)
	}))
	defer endpoint.Close()
	for _, version := range []int{4, 6} {
		t.Run(string(rune('0'+version)), func(t *testing.T) {
			inbound, outbound := protocolPeerOptions(t, "snell")
			if version == 6 {
				inbound["version"], outbound["version"] = 6, 6
			}
			startProtocolPeerCore(t, inbound, true)
			for _, network := range []string{"udp", "tcp"} {
				outbound["network"] = network
				config, err := json.Marshal(map[string]any{"log": map[string]any{"level": "error"}, "outbounds": []any{outbound}})
				if err != nil {
					t.Fatal(err)
				}
				before := requests.Load()
				response, err := (&server{}).Test(context.Background(), &gen.TestReq{
					Mode: gen.TestMode_UrlTest, Config: &gen.LoadConfigReq{CoreConfig: string(config)},
					Url: endpoint.URL, Timeout: 3000,
				})
				if err != nil {
					t.Fatal(err)
				}
				if network == "udp" {
					if response.FailureCategory != gen.FailureCategory_FailureUnsupported || response.Error == "" || requests.Load() != before {
						t.Fatalf("UDP-only URL test was not rejected before requesting the target: %+v", response)
					}
				} else if response.Error != "" || response.FailureCategory != gen.FailureCategory_FailureNone || requests.Load() <= before {
					t.Fatalf("TCP-capable URL test did not forward: %+v", response)
				}
			}
		})
	}
}
