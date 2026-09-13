package main

import (
	"context"
	"fmt"
	"grpc_server"
	"grpc_server/gen"
	"grpc_server/probe"
	"log"

	"github.com/sagernet/sing-box/experimental/v2rayapi"
)

type server struct{ grpc_server.BaseServer }

func (s *server) Start(ctx context.Context, in *gen.LoadConfigReq) (*gen.ErrorResp, error) {
	instanceMu.Lock()
	defer instanceMu.Unlock()
	out := &gen.ErrorResp{}
	if err := ctx.Err(); err != nil {
		out.Error = err.Error()
		return out, nil
	}
	if instance != nil {
		out.Error = "instance already started"
		return out, nil
	}
	i, err := NewBoxInstance([]byte(in.CoreConfig), in.StatsOutbounds)
	if err != nil {
		out.Error = err.Error()
	} else if err = ctx.Err(); err != nil {
		i.Close()
		out.Error = err.Error()
	} else {
		instance = i
	}
	return out, nil
}

func (s *server) Stop(ctx context.Context, in *gen.EmptyReq) (*gen.ErrorResp, error) {
	instanceMu.Lock()
	defer instanceMu.Unlock()
	out := &gen.ErrorResp{}
	if instance != nil {
		if err := instance.Close(); err != nil {
			out.Error = err.Error()
		}
		instance = nil
	}
	return out, nil
}

func (s *server) Test(ctx context.Context, in *gen.TestReq) (out *gen.TestResp, rpcErr error) {
	out = &gen.TestResp{}
	defer func() { grpc_server.AttachTestIdentity(out, coreCapabilities()) }()
	if err := ctx.Err(); err != nil {
		grpc_server.SetTestError(out, err, gen.FailureCategory_FailureCancelled)
		return out, nil
	}
	var err error
	switch in.Mode {
	case gen.TestMode_TcpPing:
		out.Ms, err = probe.TCP(ctx, in.Address, in.Timeout)
	case gen.TestMode_UrlTest, gen.TestMode_FullTest:
		i := currentInstance()
		if in.Config != nil {
			i, err = NewBoxInstance([]byte(in.Config.CoreConfig), nil)
			if err != nil {
				grpc_server.SetTestError(out, err, grpc_server.LocalFailure(ctx, err))
				return out, nil
			}
			defer i.Close()
		}
		if i == nil {
			out.Error = "core is not running"
			out.FailureCategory = gen.FailureCategory_FailureCore
			return out, nil
		}
		if in.Mode == gen.TestMode_FullTest {
			out, rpcErr = grpc_server.DoFullTest(ctx, in, i)
			if i.closed.Load() && out.Error != "" && ctx.Err() == nil {
				out.FailureCategory = gen.FailureCategory_FailureCore
			}
			return out, rpcErr
		}
		client := createProxyHttpClient(i)
		defer client.CloseIdleConnections()
		out.Ms, err = probe.URL(ctx, client, in.Url, in.Timeout)
		if err != nil && i.closed.Load() && ctx.Err() == nil {
			grpc_server.SetTestError(out, err, gen.FailureCategory_FailureCore)
			return out, nil
		}
	default:
		out.Error = "unknown test mode"
		out.FailureCategory = gen.FailureCategory_FailureConfig
	}
	if err != nil {
		grpc_server.SetTestError(out, err, grpc_server.RemoteFailure(ctx, err))
	}
	return out, nil
}

func (s *server) QueryStats(ctx context.Context, in *gen.QueryStatsReq) (*gen.QueryStatsResp, error) {
	instanceMu.RLock()
	defer instanceMu.RUnlock()
	out := &gen.QueryStatsResp{}
	if instance != nil && instance.stats != nil {
		response, err := instance.stats.GetStats(ctx, &v2rayapi.GetStatsRequest{
			Name: fmt.Sprintf("outbound>>>%s>>>traffic>>>%s", in.Tag, in.Direct), Reset_: true,
		})
		if err == nil && response.Stat != nil {
			out.Traffic = response.Stat.Value
		}
	}
	return out, nil
}

func (s *server) ListConnections(ctx context.Context, in *gen.EmptyReq) (*gen.ListConnectionsResp, error) {
	// The 3.26 sing-box backend did not expose individual connections here.
	return &gen.ListConnectionsResp{NekorayConnectionsJson: "[]"}, nil
}

func logClose(i *BoxInstance) {
	if err := i.Close(); err != nil {
		log.Println("close sing-box:", err)
	}
}
