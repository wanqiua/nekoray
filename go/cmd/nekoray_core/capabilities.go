package main

import (
	"context"
	"grpc_server"
	"grpc_server/gen"
	"strings"

	"github.com/xtls/xray-core/core"
)

func coreCapabilities() *gen.CapabilitiesResp {
	return grpc_server.CoreCapabilities("xray", core.Version(), "github.com/xtls/xray-core", []string{
		"rpc.check", "rpc.capabilities", "rpc.structured-test-results",
		"outbound.socks", "outbound.http", "outbound.shadowsocks", "outbound.vmess", "outbound.vless", "outbound.trojan", "outbound.wireguard",
		"transport.tcp", "transport.ws", "transport.grpc", "transport.http", "transport.kcp", "transport.quic", "transport.httpupgrade", "transport.xhttp",
		"tls", "tls.insecure", "tls.utls", "tls.reality", "tls.certificate-pin",
		"shadowsocks.rc4-md5", "shadowsocks.bf-cfb", "shadowsocks.sip003", "shadowsocks.2022",
	})
}

func (s *server) Capabilities(ctx context.Context, in *gen.EmptyReq) (*gen.CapabilitiesResp, error) {
	return coreCapabilities(), nil
}

func checkXrayConfig(input string) (string, error) {
	normalized, err := normalizeXrayConfig(input)
	if err != nil {
		return "", err
	}
	// Do not call core.New here. In the compatibility fork it replaces
	// the process log handler and a SIP003 constructor can start a child core.
	// LoadConfig uses the exact JSON decoder/builders used by the real start,
	// without constructing runtime handlers or acquiring any listener.
	if _, err = core.LoadConfig("json", strings.NewReader(normalized)); err != nil {
		return "", err
	}
	return grpc_server.ConfigHash([]byte(normalized)), nil
}

func (s *server) Check(ctx context.Context, in *gen.LoadConfigReq) (*gen.CheckResp, error) {
	response := grpc_server.CheckResponse(coreCapabilities())
	if err := ctx.Err(); err != nil {
		response.Error = err.Error()
		response.FailureCategory = gen.FailureCategory_FailureCancelled
		return response, nil
	}
	hash, err := checkXrayConfig(in.GetCoreConfig())
	if err == nil {
		err = ctx.Err()
	}
	if err != nil {
		response.Error = err.Error()
		response.FailureCategory = grpc_server.LocalFailure(ctx, err)
	} else {
		response.ConfigHash = hash
	}
	return response, nil
}
