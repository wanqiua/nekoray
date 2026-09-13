package main

import (
	"context"
	"grpc_server"
	"grpc_server/gen"

	box "github.com/sagernet/sing-box"
	"github.com/sagernet/sing-box/log"
)

var compiledFeatures = []string{
	"rpc.check", "rpc.capabilities", "rpc.structured-test-results",
	"outbound.socks", "outbound.http", "outbound.shadowsocks", "outbound.vmess", "outbound.vless", "outbound.trojan",
	"outbound.anytls", "outbound.snell", "outbound.ssh", "outbound.shadowtls",
	"transport.tcp", "transport.ws", "transport.grpc", "transport.http", "transport.httpupgrade",
	"tls", "tls.insecure", "shadowsocks.rc4-md5", "shadowsocks.sip003", "shadowsocks.2022",
	"snell.v4", "snell.v6", "tun",
}

func coreCapabilities() *gen.CapabilitiesResp {
	return grpc_server.CoreCapabilities("sing-box", singBoxVersion(), "github.com/sagernet/sing-box", compiledFeatures)
}

func (s *server) Capabilities(ctx context.Context, in *gen.EmptyReq) (*gen.CapabilitiesResp, error) {
	return coreCapabilities(), nil
}

// checkBoxConfig deliberately never calls Start, PreStart, setupCore, or the
// application's instance state. In the pinned sing-box version, public proxy
// listeners, TUN/route changes, system proxy, API listeners and remote dials
// activate in Start. Box.New performs the same local constructor validation as
// the upstream `check` command and Close releases only this unstarted box.
func checkBoxConfig(parent context.Context, input []byte) (string, error) {
	ctx, cancel, options, err := boxOptionsContext(parent, input)
	defer cancel()
	if err != nil {
		return "", err
	}
	// Validate logging options too, but suppress preflight diagnostics so node
	// tags and configuration values never enter the process log during Check.
	if !options.Log.Disabled && options.Log.Level != "" {
		if _, err = log.ParseLevel(options.Log.Level); err != nil {
			return "", err
		}
	}
	quietLog := *options.Log
	quietLog.Disabled = true
	options.Log = &quietLog
	checking, err := box.New(box.Options{Context: ctx, Options: options})
	if err != nil {
		return "", err
	}
	if err = checking.Close(); err != nil {
		return "", err
	}
	if err = parent.Err(); err != nil {
		return "", err
	}
	// Include the original JSON because legacy sniff overrides intentionally
	// survive outside the normalized upstream options. Never return the JSON.
	return grpc_server.ConfigHash(input), nil
}

func (s *server) Check(ctx context.Context, in *gen.LoadConfigReq) (*gen.CheckResp, error) {
	response := grpc_server.CheckResponse(coreCapabilities())
	if err := ctx.Err(); err != nil {
		response.Error = err.Error()
		response.FailureCategory = gen.FailureCategory_FailureCancelled
		return response, nil
	}
	hash, err := checkBoxConfig(ctx, []byte(in.GetCoreConfig()))
	if err != nil {
		response.Error = err.Error()
		response.FailureCategory = grpc_server.LocalFailure(ctx, err)
	} else {
		response.ConfigHash = hash
	}
	return response, nil
}
