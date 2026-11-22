package main

import (
	"fmt"
	"grpc_server/testutil"
	"testing"
)

func legacyGUIConfig(port int) string {
	return fmt.Sprintf(`{
		"log":{"level":"error"},
		"dns":{"servers":[
			{"tag":"dns-remote","address":"8.8.8.8","address_resolver":"dns-local","strategy":"prefer_ipv4","detour":"proxy"},
			{"tag":"dns-direct","address":"223.5.5.5","address_resolver":"dns-local","detour":"direct"},
			{"tag":"dns-local","address":"underlying://0.0.0.0","detour":"direct"},
			{"tag":"dns-block","address":"rcode://success"}],
			"rules":[{"query_type":[32,33],"server":"dns-block"}],"independent_cache":true},
		"inbounds":[{"tag":"mixed-in","type":"mixed","listen":"127.0.0.1","listen_port":%d,"sniff":true,"sniff_override_destination":false,"domain_strategy":""}],
		"outbounds":[{"tag":"proxy","type":"direct"},{"tag":"direct","type":"direct"},{"tag":"block","type":"block"},{"tag":"dns-out","type":"dns"}],
		"route":{"geoip":{"path":"unused-geoip.db"},"geosite":{"path":"unused-geosite.db"},"rules":[{"protocol":"dns","outbound":"dns-out"},{"geoip":["private"],"outbound":"proxy"}],"final":"proxy"}
	}`, port)
}

func TestGUIProtocolLifecycle(t *testing.T) {
	testutil.RunLifecycle(t, &server{}, legacyGUIConfig, true)
}

func TestModernConfigAccepted(t *testing.T) {
	config := []byte(`{"log":{"level":"error"},"dns":{"servers":[{"type":"local","tag":"local"}]},"outbounds":[{"type":"direct","tag":"proxy"}],"route":{"default_domain_resolver":"local"}}`)
	i, err := NewBoxInstance(config, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer i.Close()
}

func TestLegacyTUNAndDNSConfig(t *testing.T) {
	config := []byte(`{"dns":{"fakeip":{"enabled":true,"inet4_range":"198.18.0.0/15","inet6_range":"fc00::/18"},"servers":[{"tag":"dns-local","address":"local"},{"tag":"dns-fake","address":"fakeip"}],"rules":[{"inbound":"tun-in","server":"dns-fake"}]},"inbounds":[{"type":"tun","tag":"tun-in","interface_name":"nekoray-tun","inet4_address":"172.19.0.1/28","inet6_address":"fdfe:dcba:9876::1/126","auto_route":true,"endpoint_independent_nat":true,"sniff":true,"domain_strategy":"prefer_ipv4"}],"outbounds":[{"type":"direct","tag":"proxy"}],"route":{"auto_detect_interface":true,"final":"proxy"}}`)
	_, cancel, _, err := boxOptions(config)
	defer cancel()
	if err != nil {
		t.Fatal(err)
	}
}
